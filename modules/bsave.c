// This file is part of Deark.
// Copyright (C) 2016 Jason Summers
// See the file COPYING for terms of use.

// BSAVE image format

#include <deark-config.h>
#include <deark-private.h>
DE_DECLARE_MODULE(de_module_bsave);

#define BSAVE_HDRSIZE 7

struct localctx_struct;
typedef struct localctx_struct lctx;

typedef void (*decoder_fn_type)(deark *c, lctx *d);

#define FMT_UNKNOWN 0
#define FMT_CHAR    1
#define FMT_CGA2    2
#define FMT_CGA4    3
#define FMT_CGA16   4
#define FMT_MCGA    5
#define FMT_WH2     6
#define FMT_WH4     7
#define FMT_WH16    8
#define FMT_WH256   9
#define FMT_2COL    10
#define FMT_4COL    11
#define FMT_B265    12

struct localctx_struct {
	u32 load_segment;
	u32 load_offset;
	u32 load_addr;
	i64 data_size;

	int have_pcpaint_palinfo;
	u8 pcpaint_pal_num;
	u8 pcpaint_border_col;

	UI fmt_id;
	u8 interlaced;
	u8 has_dimension_fields;

	u8 need_id_warning;
	decoder_fn_type decoder_fn;

	int pal_valid;
	de_color pal[256];
};

struct bsave_fmt_arr_item {
	UI fmt_id;
#define FMTFLAG_HAS_DIMENSION_FIELDS 0x01
#define FMTFLAG_INTERLACED           0x02
	UI flags;
	decoder_fn_type decoder_fn;
};

// Return a width that might be overridden by user request.
static i64 get_width(deark *c, lctx *d, i64 default_width)
{
	const char *s;
	s = de_get_ext_option(c, "bsave:width");
	if(!s) s = de_get_ext_option(c, "width");
	if(s) return de_atoi64(s);
	return default_width;
}

static i64 get_height(deark *c, lctx *d, i64 default_height)
{
	const char *s;
	s = de_get_ext_option(c, "bsave:height");
	if(!s) s = de_get_ext_option(c, "height");
	if(s) return de_atoi64(s);
	return default_height;
}

// 16-color 160x100 (maybe up to 160x102) mode.
// This is really a text mode, and can be processed by do_char() as well.
static void do_cga16(deark *c, lctx *d)
{
	de_bitmap *img = NULL;
	i64 w, h;
	i64 max_possible_height;
	i64 i, j;
	u8 charcode, colorcode;
	i64 src_rowspan;
	u8 color0, color1;
	int charwarning = 0;

	de_declare_fmt(c, "BSAVE-PC 16-color CGA pseudo-graphics");

	w = get_width(c, d, 160);
	h = get_height(c, d, 100);

	// Every pair of bytes codes for two pixels; i.e. one byte per pixel.
	src_rowspan = w;
	max_possible_height = (d->data_size+src_rowspan-1)/src_rowspan;
	if(h > max_possible_height)
		h = max_possible_height;

	if(h < 1) {
		de_err(c, "Not enough data for this format");
		goto done;
	}

	de_dbg_dimensions(c, w, h);
	if(!de_good_image_dimensions(c, w, h)) goto done;
	img = de_bitmap_create(c, w, h, 3);

	de_copy_std_palette(DE_PALID_PC16, 0, 0, 16, d->pal, 16, 0);

	for(j=0; j<h; j++) {
		for(i=0; i<w; i+=2) {
			charcode = de_getbyte(BSAVE_HDRSIZE + j*src_rowspan + i);
			colorcode = de_getbyte(BSAVE_HDRSIZE + j*src_rowspan + i+1);

			if(charwarning==0 && charcode!=0xdd && charcode!=0xde) {
				// TODO: We could also handle space characters and full-block characters,
				// at least. But maybe not worth the trouble.
				de_warn(c, "Unexpected code found (0x%02x). Format may not be correct.", (UI)charcode);
				charwarning=1;
			}

			if(charcode==0xde) {
				color0 = colorcode>>4;
				color1 = colorcode&0x0f;
			}
			else {
				color1 = colorcode>>4;
				color0 = colorcode&0x0f;
			}

			de_bitmap_setpixel_rgb(img, i+0, j, d->pal[(UI)color0]);
			de_bitmap_setpixel_rgb(img, i+1, j, d->pal[(UI)color1]);
		}
	}

	de_bitmap_write_to_file(img, NULL, 0);
done:
	de_bitmap_destroy(img);
}

// 4-color interlaced or non-interlaced
// "wh4": http://cd.textfiles.com/bthevhell/200/111/ - *.pic
static void do_4color(deark *c, lctx *d)
{
	// TODO: This may not be the right palette.
	static const de_color default_palette[4] = { 0x000000, 0x55ffff, 0xff55ff, 0xffffff };
	de_color palette[4];
	int palent;
	i64 w, h;
	i64 i,j;
	i64 pos;
	i64 src_rowspan;
	de_bitmap *img = NULL;

	if(d->has_dimension_fields) {
		if(d->interlaced)
			de_declare_fmt(c, "BSAVE-PC 4-color, interlaced, 11-byte header");
		else
			de_declare_fmt(c, "BSAVE-PC 4-color, noninterlaced, 11-byte header");
	}
	else {
		if(d->interlaced)
			de_declare_fmt(c, "BSAVE-PC 4-color, interlaced");
		else
			de_declare_fmt(c, "BSAVE-PC 4-color, noninterlaced");
	}

	pos = BSAVE_HDRSIZE;

	if(d->has_dimension_fields) {
		// 11-byte header that includes width & height
		w = (de_getu16le(pos) + 1)/2; // width = number of bits??
		h = de_getu16le(pos+2);
		pos+=4;
	}
	else if(!d->interlaced) {
		w = get_width(c, d, 320);
		src_rowspan = (w+3)/4;
		h = d->data_size / src_rowspan;
		h = get_height(c, d, h);
	}
	else {
		w = get_width(c, d, 320);
		h = get_height(c, d, 200);	// TODO: Calculate this?
	}

	// Set the palette
	if(d->have_pcpaint_palinfo) {
		de_copy_std_palette(DE_PALID_CGA, (int)d->pcpaint_pal_num, 0, 4, palette, 4, 0);
		palette[0] = de_get_std_palette_entry(DE_PALID_PC16, 0, (int)d->pcpaint_border_col);
	}
	else {
		for(i=0; i<4; i++) {
			palette[i] = default_palette[i];
		}
	}

	src_rowspan = (w+3)/4;
	de_dbg_dimensions(c, w, h);
	if(!de_good_image_dimensions(c, w, h)) goto done;
	img = de_bitmap_create(c, w, h, 3);

	for(j=0;j<h;j++) {
		for(i=0;i<w;i++) {
			if(d->interlaced) {
				// Image is interlaced. Even-numbered scanlines are stored first.
				palent = (int)de_get_bits_symbol(c->infile, 2,
					pos + (j%2)*8192 + (j/2)*src_rowspan, i);
			}
			else {
				palent = (int)de_get_bits_symbol(c->infile, 2,
					pos + j*src_rowspan, i);
			}
			de_bitmap_setpixel_rgb(img, i, j, palette[palent]);
		}
	}

	de_bitmap_write_to_file(img, NULL, 0);
done:
	de_bitmap_destroy(img);
}

// 2-color interlaced or non-interlaced
// "cga2": http://cd.textfiles.com/bthevhell/100/21/
// "wh2": http://cd.textfiles.com/bthevhell/200/112/
static void do_2color(deark *c, lctx *d)
{
	i64 w, h;
	i64 j;
	i64 src_rowspan;
	i64 pos;
	de_bitmap *img = NULL;

	pos = BSAVE_HDRSIZE;

	if(d->has_dimension_fields) {
		if(d->interlaced)
			de_declare_fmt(c, "BSAVE-PC 2-color, interlaced, 11-byte header");
		else
			de_declare_fmt(c, "BSAVE-PC 2-color, noninterlaced, 11-byte header");
	}
	else {
		if(d->interlaced)
			de_declare_fmt(c, "BSAVE-PC 2-color, interlaced");
		else
			de_declare_fmt(c, "BSAVE-PC 2-color, noninterlaced");
	}

	if(d->has_dimension_fields) {
		// 11-byte header that includes width & height
		w = de_getu16le(pos);
		h = de_getu16le(pos+2);
		pos+=4;
	}
	else if(!d->interlaced) {
		w = get_width(c, d, 640);
		src_rowspan = (w+7)/8;
		h = d->data_size / src_rowspan;
		h = get_height(c, d, h);
	}
	else {
		w = get_width(c, d, 640);
		h = get_height(c, d, 200);
	}

	de_dbg_dimensions(c, w, h);
	src_rowspan = (w+7)/8;

	if(!de_good_image_dimensions(c, w, h)) goto done;
	img = de_bitmap_create(c, w, h, 1);

	for(j=0; j<h; j++) {
		if(d->interlaced) {
			de_convert_row_bilevel(c->infile, pos + (j%2)*8192 + (j/2)*src_rowspan,
				img, j, 0);
		}
		else {
			de_convert_row_bilevel(c->infile, pos + j*src_rowspan, img, j, 0);
		}
	}

	de_bitmap_write_to_file(img, NULL, 0);
done:
	de_bitmap_destroy(img);
}

// 256-color
// http://cd.textfiles.com/advheaven2/PUZZLES/DRCODE12/
static void do_256color(deark *c, lctx *d)
{
	i64 w, h;
	i64 pos = BSAVE_HDRSIZE;
	de_bitmap *img = NULL;

	de_declare_fmt(c, "BSAVE-PC 256-color");

	if(d->has_dimension_fields) {
		w = de_getu16le_p(&pos);
		w = (w+7)/8;
		h = de_getu16le_p(&pos);
	}
	else {
		w = get_width(c, d, 320);
		h = d->data_size / w;
		h = get_height(c, d, h);
	}
	de_dbg_dimensions(c, w, h);

	if(!d->pal_valid) {
		de_copy_std_palette(DE_PALID_VGA256, 0, 0, 256, d->pal, 256, 0);
	}

	if(!de_good_image_dimensions(c, w, h)) goto done;
	img = de_bitmap_create(c, w, h, 3);
	de_convert_image_paletted(c->infile, pos, 8, w, d->pal, img, 0);
	de_bitmap_write_to_file(img, NULL, 0);
done:
	de_bitmap_destroy(img);
}

// 11-byte header that includes width & height, 16 color, inter-row interlaced
// http://cd.textfiles.com/advheaven2/SOLITAIR/SP107/
static void do_wh16(deark *c, lctx *d)
{
	de_bitmap *img = NULL;
	i64 w, h;
	i64 src_planespan;
	i64 src_rowspan;
	i64 pos;

	de_declare_fmt(c, "BSAVE-PC 16-color, 4-plane, 11-byte header");

	pos = BSAVE_HDRSIZE;
	w = de_getu16le_p(&pos);
	h = de_getu16le_p(&pos);
	de_dbg_dimensions(c, w, h);
	if(!de_good_image_dimensions(c, w, h)) goto done;

	img = de_bitmap_create(c, w, h, 3);
	src_planespan = (w+7)/8;
	src_rowspan = src_planespan*4;

	de_copy_std_palette(DE_PALID_PC16, 0, 0, 16, d->pal, 16, 0);
	de_convert_image_paletted_planar(c->infile, pos, 4, src_rowspan, src_planespan,
		d->pal, img, 0x2);
	de_bitmap_write_to_file(img, NULL, 0);

done:
	de_bitmap_destroy(img);
}

// Used at http://cd.textfiles.com/bthevhell/300/265/
// A strange 2-bits/2-pixel color format.
static void do_b265(deark *c, lctx *d)
{
	static const de_color palette1[4] = { 0xffffff, 0x55ffff, 0x000000, 0xffffff };
	static const de_color palette2[4] = { 0xffffff, 0x000000, 0x000000, 0x000000 };
	int palent;
	i64 w, h;
	i64 i,j;
	i64 bits_per_scanline;
	de_bitmap *img = NULL;
	i64 fakewidth;

	de_declare_fmt(c, "BSAVE-PC special");

	w = 320;
	fakewidth = w/2;
	h = d->data_size * 4 / fakewidth;
	bits_per_scanline = w;

	de_dbg_dimensions(c, w, h);
	if(!de_good_image_dimensions(c, w, h)) goto done;
	img = de_bitmap_create(c, w, h, 3);

	for(j=0; j<h; j++) {
		for(i=0; i<fakewidth; i++) {
			palent = (int)de_get_bits_symbol(c->infile, 2,
				BSAVE_HDRSIZE + (j/8)*bits_per_scanline + (i/4)*8 + j%8, i%4);
			de_bitmap_setpixel_rgb(img, 2*i  , j, palette1[palent]);
			de_bitmap_setpixel_rgb(img, 2*i+1, j, palette2[palent]);
		}
	}

	de_bitmap_write_to_file(img, NULL, 0);

done:
	de_bitmap_destroy(img);
}

static void do_char_1screen(deark *c, lctx *d, struct de_char_screen *screen, i64 pgnum,
	i64 pg_offset_in_data, i64 width, i64 height)
{
	i64 i, j;
	unsigned int ch;
	u8 fgcol, bgcol;
	i64 offset;
	u8 b0, b1;
	struct de_encconv_state es;

	screen->width = width;
	screen->height = height;
	screen->cell_rows = de_mallocarray(c, height, sizeof(struct de_char_cell*));
	de_encconv_init(&es, DE_ENCODING_CP437_G);

	for(j=0; j<height; j++) {
		screen->cell_rows[j] = de_mallocarray(c, width, sizeof(struct de_char_cell));

		for(i=0; i<width; i++) {
			// 96 padding bytes per page?
			offset = BSAVE_HDRSIZE + pg_offset_in_data + j*(width*2) + i*2;

			b0 = de_getbyte(offset);
			b1 = de_getbyte(offset+1);

			ch = b0;
			//"The attribute byte stores the foreground color in the low nibble and the background color and blink attribute in the high nibble."
			//TODO: "blink" attribute?
			fgcol = (b1 & 0x0f);
			bgcol = (b1 & 0xf0) >> 4;

			screen->cell_rows[j][i].fgcol = (de_color)fgcol;
			screen->cell_rows[j][i].bgcol = (de_color)bgcol;
			screen->cell_rows[j][i].codepoint = (i32)ch;
			screen->cell_rows[j][i].codepoint_unicode = de_char_to_unicode_ex((i32)ch, &es);
		}
	}
}

static void do_char(deark *c, lctx *d)
{
	struct de_char_context *charctx = NULL;
	i64 numpages;
	i64 pgnum;
	i64 width, height;
	i64 height_for_this_page;
	i64 bytes_per_page;
	i64 bytes_for_this_page;
	i64 pg_offset_in_data;

	de_declare_fmt(c, "BSAVE-PC character graphics");

	width = get_width(c, d, 80);
	height = get_height(c, d, 25);

	// If there are multiple pages, the usually have some unused space between
	// them. Try to guess how much.

	if(width*height*2 <= 2048) {
		bytes_per_page = 2048; // E.g. 40x25(*2) = 2000
	}
	else if(width*height*2 <= 4096) {
		bytes_per_page = 4096; // E.g. 80x25(*2) = 4000
	}
	else if(width*height*2 <= 8192) {
		bytes_per_page = 8192; // Just guessing. Maybe 80x50.
	}
	else {
		bytes_per_page = 16384; // E.g. 80x100 (160x100) pseudo-graphics mode.
	}

	numpages = (d->data_size + (bytes_per_page-1))/bytes_per_page;
	if(numpages<1) {
		goto done;
	}
	de_dbg(c, "pages: %d", (int)numpages);

	charctx = de_malloc(c, sizeof(struct de_char_context));
	charctx->nscreens = numpages;
	charctx->screens = de_mallocarray(c, numpages, sizeof(struct de_char_screen*));

	de_copy_std_palette(DE_PALID_PC16, 0, 0, 16, charctx->pal, 16, 0);

	for(pgnum=0; pgnum<numpages; pgnum++) {
		charctx->screens[pgnum] = de_malloc(c, sizeof(struct de_char_screen));

		pg_offset_in_data = bytes_per_page*pgnum;
		bytes_for_this_page = d->data_size - pg_offset_in_data;
		if(bytes_for_this_page<2) break;

		// Reduce the height if there's not enough data for it.
		height_for_this_page = (bytes_for_this_page+(width*2-1)) / (width*2);
		if(height_for_this_page>height) {
			height_for_this_page = height;
		}

		do_char_1screen(c, d, charctx->screens[pgnum], pgnum, pg_offset_in_data, width, height_for_this_page);
	}

	de_char_output_to_file(c, charctx);

done:
	de_free_charctx(c, charctx);
}

struct bsave_fmt_name_item {
	UI fmt_id;
	const char *name;
};

static const struct bsave_fmt_arr_item bsave_fmt_arr[] = {
	{FMT_CHAR,  0x00, do_char    },
	{FMT_CGA2,  0x02, do_2color  },
	{FMT_CGA4,  0x02, do_4color  },
	{FMT_CGA16, 0x00, do_cga16   },
	{FMT_MCGA,  0x00, do_256color},
	{FMT_WH2,   0x01, do_2color  },
	{FMT_WH4,   0x01, do_4color  },
	{FMT_WH16,  0x00, do_wh16    },
	{FMT_WH256, 0x01, do_256color},
	{FMT_2COL,  0x00, do_2color  },
	{FMT_4COL,  0x00, do_4color  },
	{FMT_B265,  0x00, do_b265    }
};

static const struct bsave_fmt_arr_item *get_fmt_info_by_id(UI fmt_id)
{
	size_t k;

	for(k=0; k<DE_ARRAYCOUNT(bsave_fmt_arr); k++) {
		if(bsave_fmt_arr[k].fmt_id==fmt_id) {
			return &bsave_fmt_arr[k];
		}
	}
	return NULL;
}

static int do_read_palette_file(deark *c, lctx *d, const char *palfn)
{
	dbuf *f = NULL;
	int retval = 0;
	i64 i;
	u8 buf[3];

	de_dbg(c, "reading palette file %s", palfn);

	f = dbuf_open_input_file(c, palfn);
	if(!f) {
		de_err(c, "Cannot read palette file %s", palfn);
		goto done;
	}

	for(i=0; i<256; i++) {
		dbuf_read(f, buf, BSAVE_HDRSIZE + 3*i, 3);
		d->pal[i] = DE_MAKE_RGB(de_scale_63_to_255(buf[0]),
			de_scale_63_to_255(buf[1]),
			de_scale_63_to_255(buf[2]));
	}
	d->pal_valid = 1;

	retval = 1;
done:
	dbuf_close(f);
	return retval;
}

struct bsave_id_info {
	u8 might_be_bsave;
	u8 probably_not_bsave;
	u8 pcpaint_sig_found;
	i64 pcpaint_sig_pos;
};

static void identify_bsave_internal(deark *c, struct bsave_id_info *idi)
{
	u32 load_segment;
	i64 data_size;
	// Knwon signatures: "PCPaint V1.0", "PCPaint V1.5".
	static const u8 *pcpaintsig = (const u8*)"PCPaint V1.";

	de_zeromem(idi, sizeof(struct bsave_id_info));
	if(c->infile->len<12) return;
	if(de_getbyte(0)!=0xfd) return;
	idi->might_be_bsave = 1;

	load_segment = (u32)de_getu16le(1);
	data_size = de_getu16le(5);
	if(data_size==0) {
		idi->probably_not_bsave = 1;
		return;
	}

	if(load_segment==0xb800 && (c->infile->len>=8007+14)) {
		i64 pos;

		pos = 8007;
		if(!dbuf_memcmp(c->infile, pos, pcpaintsig, 11)) {
			idi->pcpaint_sig_found = 1;
			idi->pcpaint_sig_pos = pos;
			return;
		}
	}

	if(data_size > c->infile->len) {
		idi->probably_not_bsave = 1;
	}
}

static void read_pcpaint_palinfo(deark *c, lctx *d, struct bsave_id_info *idi)
{
	i64 pos;

	if(!idi->pcpaint_sig_found) return;
	pos = idi->pcpaint_sig_pos;
	d->have_pcpaint_palinfo = 1;
	de_dbg(c, "PCPaint settings found at %"I64_FMT, pos);
	de_dbg_indent(c, 1);
	pos += 12;
	d->pcpaint_pal_num = de_getbyte_p(&pos);
	de_dbg(c, "palette: %u", (UI)d->pcpaint_pal_num);
	d->pcpaint_border_col = de_getbyte_p(&pos);
	de_dbg(c, "border color: %u", (UI)d->pcpaint_border_col);
	de_dbg_indent(c, -1);
}

static const struct bsave_fmt_name_item bsave_fmt_name_arr[] = {
	{FMT_CHAR,  "char" },
	{FMT_CGA2,  "cga2" },
	{FMT_CGA4,  "cga4" },
	{FMT_CGA16, "cga16"},
	{FMT_MCGA,  "mcga" },
	{FMT_WH2,   "wh2"  },
	{FMT_WH4,   "wh4"  },
	{FMT_WH16,  "wh16" },
	{FMT_WH256, "wh256"},
	{FMT_2COL,  "2col" },
	{FMT_4COL,  "4col" },
	{FMT_B265,  "b265" }
};

static UI bsave_fmt_name_to_id(const char *name)
{
	size_t k;

	if(!name) return FMT_UNKNOWN;

	for(k=0; k<DE_ARRAYCOUNT(bsave_fmt_name_arr); k++) {
		const struct bsave_fmt_name_item *item;

		item = &bsave_fmt_name_arr[k];
		if(!de_strcmp(name, item->name)) {
			return item->fmt_id;
		}
	}
	return FMT_UNKNOWN;
}

static void use_fmt_by_id(deark *c, lctx *d, UI fmt_id)
{
	const struct bsave_fmt_arr_item *fmt_info;

	fmt_info = get_fmt_info_by_id(fmt_id);
	if(fmt_info) {
		d->decoder_fn = fmt_info->decoder_fn;
		d->has_dimension_fields = (fmt_info->flags & FMTFLAG_HAS_DIMENSION_FIELDS)?1:0;
		d->interlaced = (fmt_info->flags & FMTFLAG_INTERLACED)?1:0;
	}
}

static void detect_bsave_fmt(deark *c, lctx *d, struct bsave_id_info *idi)
{
	d->need_id_warning = 1; // deafult

	if(idi->pcpaint_sig_found) {
		d->interlaced = 1;
		d->decoder_fn = do_4color;
		// Most files with a signature are cga4, but I found a few that are cga2.
		// There are also images that are contrived to work in either mode.
		// I'm guessing that these are the only two possibilities.
		de_info(c, "Note: If the image doesn't look right, try \"-opt bsave:fmt=cga2\".");
		d->need_id_warning = 0;
	}

	// TODO: Better autodetection. This barely does anything.
	if(d->load_segment==0xb800 && (d->data_size==16384 || d->data_size==16383)) {
		d->interlaced = 1;
		d->decoder_fn = do_4color;
	}
	else if(d->load_segment==0xa000 && d->data_size==64000) {
		d->decoder_fn = do_256color;
	}
}

static void de_run_bsave(deark *c, de_module_params *mparams)
{
	const char *fmtname_req;
	const char *s;
	lctx *d;
	struct bsave_id_info idi;

	d = de_malloc(c, sizeof(lctx));
	d->fmt_id = FMT_UNKNOWN;
	identify_bsave_internal(c, &idi);
	if(!idi.might_be_bsave) {
		de_err(c, "Not a BSAVE image file");
		goto done;
	}
	if(idi.probably_not_bsave) {
		if(c->module_disposition==DE_MODDISP_EXPLICIT) {
			de_warn(c, "This is probably not a BSAVE image file");
		}
	}

	d->load_segment = (u32)de_getu16le(1);
	d->load_offset = (u32)de_getu16le(3);
	d->load_addr = d->load_segment*16 + d->load_offset;
	d->data_size = de_getu16le(5);

	de_dbg(c, "segment: 0x%04x", (UI)d->load_segment);
	de_dbg(c, "offset: 0x%04x", (UI)d->load_offset);
	de_dbg(c, "data size: 0x%04x (%d)", (UI)d->data_size, (int)d->data_size);

	if(d->data_size > c->infile->len) {
		de_warn(c, "Data (size %"I64_FMT") goes beyond end of file", d->data_size);
	}

	fmtname_req = de_get_ext_option(c, "bsave:fmt");
	if(fmtname_req) {
		if(!de_strcmp(fmtname_req, "auto")) {
			;
		}
		else {
			d->fmt_id = bsave_fmt_name_to_id(fmtname_req);
			if(d->fmt_id==FMT_UNKNOWN) {
				de_warn(c, "Unknown format name \"%s\"", fmtname_req);
			}
		}
	}

	read_pcpaint_palinfo(c, d, &idi);

	use_fmt_by_id(c, d, d->fmt_id);

	if(!d->decoder_fn) {
		detect_bsave_fmt(c, d, &idi);
	}

	if(!d->decoder_fn) {
		de_err(c, "Unidentified BSAVE format, try \"-opt bsave:fmt=...\". "
			"Use \"-m bsave -h\" for a list.");
		goto done;
	}

	if(d->need_id_warning) {
		de_warn(c, "BSAVE formats can't be reliably identified. You may need to "
			"use \"-opt bsave:fmt=...\". Use \"-m bsave -h\" for a list.");
	}

	s = de_get_ext_option(c, "palfile");
	if(!s) s = de_get_ext_option(c, "file2");
	if(s) {
		if(!do_read_palette_file(c, d, s)) goto done;
	}

	(void)d->decoder_fn(c, d);

done:
	de_free(c, d);
}

static int de_identify_bsave(deark *c)
{
	struct bsave_id_info idi;

	// Note - Make sure XZ has higher confidence.
	// Note - Make sure BLD has higher confidence.
	// Note - See also bsave_cmpr module.
	identify_bsave_internal(c, &idi);
	if(!idi.might_be_bsave) return 0;
	if(idi.probably_not_bsave) return 0;

	if(idi.pcpaint_sig_found) {
		return 91;
	}
	return 10;
}

static void de_help_bsave(deark *c)
{
	de_msg(c, "-opt bsave:fmt=...");
	de_msg(c, " char  : Character graphics");
	de_msg(c, " cga2  : 2-color, 640x200");
	de_msg(c, " cga4  : 4-color, 320x200");
	de_msg(c, " cga16 : 16-color, 160x100 pseudographics");
	de_msg(c, " mcga  : 256-color, 320x200");
	de_msg(c, " wh2   : 2-color, 11-byte header");
	de_msg(c, " wh4   : 4-color, 11-byte header");
	de_msg(c, " wh16  : 16-color, 4-plane, 11-byte header");
	de_msg(c, " wh256 : 256-color, 11-byte header");
	de_msg(c, " 2col  : 2-color noninterlaced");
	de_msg(c, " 4col  : 4-color noninterlaced");
	//de_msg(c, " b265  : Special"); - not documented here
}

void de_module_bsave(deark *c, struct deark_module_info *mi)
{
	mi->id = "bsave";
	mi->desc = "BSAVE/BLOAD image";
	mi->run_fn = de_run_bsave;
	mi->identify_fn = de_identify_bsave;
	mi->help_fn = de_help_bsave;
}
