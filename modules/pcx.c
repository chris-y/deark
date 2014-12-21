// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// PCX (PC Paintbrush)

#include <deark-config.h>
#include <deark-modules.h>

typedef struct localctx_struct {
	de_byte version;
	de_byte encoding;
	de_int64 bits;
	de_int64 bits_per_pixel;
	de_int64 margin_l, margin_t, margin_r, margin_b;
	de_int64 planes;
	de_int64 rowspan_raw;
	de_int64 rowspan;
	de_int64 ncolors;
	de_byte palette_info;
	de_int64 width, height;
	int has_vga_pal;
	dbuf *unc_pixels;
	de_uint32 pal[256];
} lctx;

static int do_read_header(deark *c, lctx *d)
{
	int retval = 0;

	d->version = de_getbyte(1);
	d->encoding = de_getbyte(2);
	d->bits = (de_int64)de_getbyte(3); // Bits per pixel per plane
	d->margin_l = (de_int64)de_getui16le(4);
	d->margin_t = (de_int64)de_getui16le(6);
	d->margin_r = (de_int64)de_getui16le(8);
	d->margin_b = (de_int64)de_getui16le(10);

	// The palette (offset 16-63) will be read later.

	d->planes = (de_int64)de_getbyte(0x41);
	d->rowspan_raw = (de_int64)de_getui16le(0x42);
	d->palette_info = de_getbyte(0x44);

	de_dbg(c, "format version: %d, encoding: %d, planes: %d, bits: %d\n", (int)d->version,
		(int)d->encoding, (int)d->planes, (int)d->bits);
	de_dbg(c, "bytes/plane/row: %d, palette info: %d\n", (int)d->rowspan_raw,
		(int)d->palette_info);
	de_dbg(c, "margins: %d, %d, %d, %d\n", (int)d->margin_l, (int)d->margin_t,
		(int)d->margin_r, (int)d->margin_b);

	d->width = d->margin_r - d->margin_l +1;
	d->height = d->margin_b - d->margin_t +1;
	de_dbg(c, "dimensions: %dx%d\n", (int)d->width, (int)d->height);
	if(!de_good_image_dimensions(c, d->width, d->height)) goto done;

	d->rowspan = d->rowspan_raw * d->planes;
	de_dbg(c, "bytes/row: %d\n", (int)d->rowspan);

	d->bits_per_pixel = d->bits * d->planes;

	if(d->encoding != 1) {
		de_err(c, "Unsupported encoding: %d\n", (int)d->encoding);
		goto done;
	}

	// Enumerate the known PCX image types.
	if(d->planes==1 && d->bits==1) {
		d->ncolors = 2;
	}
	//else if(d->planes==2 && d->bits==1) {
	//	d->ncolors = 4;
	//}
	else if(d->planes==1 && d->bits==2) {
		d->ncolors = 4;
	}
	else if(d->planes==3 && d->bits==1) {
		d->ncolors = 8;
	}
	else if(d->planes==4 && d->bits==1) {
		d->ncolors = 16;
	}
	//else if(d->planes==1 && d->bits==4) {
	//	d->ncolors = 16;
	//}
	//else if(d->planes==4 && d->bits==2) {
	//	d->ncolors = 16; (?)
	//}
	else if(d->planes==1 && d->bits==8) {
		d->ncolors = 256;
	}
	//else if(d->planes==4 && d->bits==4) {
	//	d->ncolors = 4096;
	//}
	else if(d->planes==3 && d->bits==8) {
		d->ncolors = 16777216;
	}
	//else if(d->planes==4 && d->bits==8) {
	//	d->ncolors = 16777216;
	//}
	else {
		de_err(c, "Unsupported image type (bits=%d, planes=%d)\n",
			(int)d->bits, (int)d->planes);
		goto done;
	}

	de_dbg(c, "number of colors: %d\n", (int)d->ncolors);

	// Sanity check
	if(d->rowspan > d->width * 3 + 100) {
		de_err(c, "Bad bytes/line (%d)\n", (int)d->rowspan_raw);
		goto done;
	}

	retval = 1;
done:
	return retval;
}

static int do_read_vga_palette(deark *c, lctx *d)
{
	de_int64 pos;
	de_int64 k;

	if(d->version<5) return 0;
	if(d->ncolors!=256) return 0;
	pos = c->infile->len - 769;
	if(pos<128) return 0;

	if(de_getbyte(pos) != 0x0c) {
		return 0;
	}

	de_dbg(c, "reading VGA palette at %d\n", (int)pos);
	d->has_vga_pal = 1;
	pos++;
	for(k=0; k<256; k++) {
		d->pal[k] = dbuf_getRGB(c->infile, pos + 3*k, 0);
	}

	return 1;
}

// This is the "default EGA palette" used by several PCX viewers.
// I don't know its origin, but it seems to be at least approximately correct.
// (8-color version-3 PCXs apparently use only the first 8 colors of this
// palette.)
static const de_uint32 ega16pal[16] = {
	0x000000,0xbf0000,0x00bf00,0xbfbf00,0x0000bf,0xbf00bf,0x00bfbf,0xc0c0c0,
	0x808080,0xff0000,0x00ff00,0xffff00,0x0000ff,0xff00ff,0x00ffff,0xffffff
};

static void do_palette_stuff(deark *c, lctx *d)
{
	de_int64 k;

	if(d->version==3 && d->ncolors>2 && d->ncolors<=16) {
		de_dbg(c, "Using default EGA palette\n");
		for(k=0; k<16; k++) {
			d->pal[k] = ega16pal[k];
		}
		return;
	}

	if(d->version>=5 && d->ncolors==256) {
		if(do_read_vga_palette(c, d)) {
			return;
		}
		de_warn(c, "Expected VGA palette was not found\n");
	}

	if(d->ncolors==4) {
		de_byte p0, p3;
		unsigned int bgcolor;
		unsigned int fgpal;

		de_warn(c, "4-color PCX images might not be supported correctly\n");
		de_dbg(c, "using a CGA palette\n");

		p0 = de_getbyte(16);
		p3 = de_getbyte(19);
		bgcolor = p0>>4;
		fgpal = p3>>5;
		de_dbg(c, "palette #%d, background color %d\n", (int)fgpal, (int)bgcolor);

		// Set first pal entry to background color
		d->pal[0] = de_palette_pc16(bgcolor);

		// TODO: These palettes are quite possibly incorrect. I can't find good
		// information about them.
		switch(fgpal) {
		case 0: case 2: // C=0 P=? I=0
			d->pal[1]=0x00aaaa; d->pal[2]=0xaa0000; d->pal[3]=0xaaaaaa; break;
		case 1: case 3: // C=0 P=? I=1
			d->pal[1]=0x55ffff; d->pal[2]=0xff5555; d->pal[3]=0xffffff; break;
		case 4: // C=1 P=0 I=0
			d->pal[1]=0x00aa00; d->pal[2]=0xaa0000; d->pal[3]=0xaa5500; break;
		case 5: // C=1 P=0 I=1
			d->pal[1]=0x55ff55; d->pal[2]=0xff5555; d->pal[3]=0xffff55; break;
		case 6: // C=1 P=1 I=0
			d->pal[1]=0x00aaaa; d->pal[2]=0xaa00aa; d->pal[3]=0xaaaaaa; break;
		case 7: // C=1 P=1 I=1
			d->pal[1]=0x55ffff; d->pal[2]=0xff55ff; d->pal[3]=0xffffff; break;
		}
		return;
	}

	de_dbg(c, "using 16-color palette from header\n");

	for(k=0; k<16; k++) {
		d->pal[k] = dbuf_getRGB(c->infile, 16 + 3*k, 0);
	}
}

static int do_uncompress(deark *c, lctx *d)
{
	de_int64 pos;
	de_byte b, b2;
	de_int64 count;
	de_int64 k;
	de_int64 expected_bytes;
	de_int64 endpos;

	pos = 128;

	expected_bytes = d->rowspan * d->height;
	d->unc_pixels = dbuf_create_membuf(c, expected_bytes);

	endpos = c->infile->len;
	if(d->has_vga_pal) {
		// The last 769 bytes of this file are reserved for the palette.
		// Don't try to decoded them as pixels.
		endpos -= 769;
	}

	while(1) {
		if(pos>=endpos) {
			break; // Reached the end of source data
		}
		if(d->unc_pixels->len >= expected_bytes) {
			break; // Reached the end of the image
		}
		b = de_getbyte(pos++);

		if(b>=0xc0) {
			count = (de_int64)(b&0x3f);
			b2 = de_getbyte(pos++);

			for(k=0; k<count; k++) {
				dbuf_writebyte(d->unc_pixels, b2);
			}
		}
		else {
			dbuf_writebyte(d->unc_pixels, b);
		}
	}

	if(d->unc_pixels->len < expected_bytes) {
		de_warn(c, "Expected %d bytes of image data, but only found %d\n",
			(int)expected_bytes, (int)d->unc_pixels->len);
	}

	return 1;
}

static void do_bitmap_1bpp(deark *c, lctx *d)
{
	// Apparently, bilevel PCX images do not use a palette and are always black
	// and white.
	// The paletted algorithm would work (if we did something about the palette)
	// but this special case is easy and efficient.
	de_convert_and_write_image_bilevel(d->unc_pixels, 0,
		d->width, d->height, d->rowspan, 0, NULL);
}

static void do_bitmap_paletted(deark *c, lctx *d)
{
	struct deark_bitmap *img = NULL;
	de_int64 i, j;
	de_int64 plane;
	de_byte b;
	unsigned int palent;

	img = de_bitmap_create(c, d->width, d->height, 3);

	for(j=0; j<d->height; j++) {
		for(i=0; i<d->width; i++) {
			palent = 0;
			for(plane=0; plane<d->planes; plane++) {
				b = de_get_bits_symbol(d->unc_pixels, (int)d->bits,
					j*d->rowspan + plane*d->rowspan_raw, i);
				palent |= b<<(plane*d->bits);
			}
			if(palent>255) palent=0; // Should be impossible.
			de_bitmap_setpixel_rgb(img, i, j, d->pal[palent]);
		}
	}

	de_bitmap_write_to_file(img, NULL);
	de_bitmap_destroy(img);
}

static void do_bitmap_24bpp(deark *c, lctx *d)
{
	struct deark_bitmap *img = NULL;
	de_int64 i, j;
	de_int64 plane;
	de_byte rgb[3];

	img = de_bitmap_create(c, d->width, d->height, 3);

	for(j=0; j<d->height; j++) {
		for(i=0; i<d->width; i++) {
			//palent = 0;
			for(plane=0; plane<3; plane++) {
				rgb[plane] = dbuf_getbyte(d->unc_pixels, j*d->rowspan + plane*d->rowspan_raw +i);
			}
			de_bitmap_setpixel_rgb(img, i, j, DE_MAKE_RGB(rgb[0], rgb[1], rgb[2]));
		}
	}

	de_bitmap_write_to_file(img, NULL);
	de_bitmap_destroy(img);
}

static void do_bitmap(deark *c, lctx *d)
{
	if(d->bits_per_pixel==1) {
		do_bitmap_1bpp(c, d);
	}
	else if(d->bits_per_pixel<=8) {
		do_bitmap_paletted(c, d);
	}
	else if(d->bits_per_pixel==24) {
		do_bitmap_24bpp(c, d);
	}
	else {
		de_err(c, "Unsupported bits/pixel: %d\n", (int)d->bits_per_pixel);
	}
}

static void de_run_pcx(deark *c, const char *params)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));

	if(!do_read_header(c, d)) {
		goto done;
	}

	do_palette_stuff(c, d);

	if(!do_uncompress(c, d)) {
		goto done;
	}

	do_bitmap(c, d);

done:
	dbuf_close(d->unc_pixels);
	de_free(c, d);
}

static int de_identify_pcx(deark *c)
{
	de_byte buf[8];

	de_read(buf, 0, 8);
	if(buf[0]==0x0a && (buf[1]==0 || buf[1]==2 || buf[1]==3
		|| buf[1]==4 || buf[1]==5))
	{
		if(de_input_file_has_ext(c, "pcx"))
			return 100;

		return 10;
	}
	return 0;
}

void de_module_pcx(deark *c, struct deark_module_info *mi)
{
	mi->id = "pcx";
	mi->run_fn = de_run_pcx;
	mi->identify_fn = de_identify_pcx;
}
