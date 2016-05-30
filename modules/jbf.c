// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// PaintShop Pro Browser Cache (JBF) (pspbrwse.jbf)

// This module was developed with the help of information from
// jbfinspect.c (https://github.com/0x09/jbfinspect), which says:
//     "The author disclaims all copyright on this code."

#include <deark-config.h>
#include <deark-private.h>
#include "fmtutil.h"
DE_DECLARE_MODULE(de_module_jbf);

struct page_ctx {
	de_finfo *fi;
	de_ucstring *fname;
	const char *thumbnail_ext;
};

typedef struct localctx_struct {
	unsigned int ver_major;
	unsigned int ver_minor;
	unsigned int ver_combined;
	de_int64 image_count;
} lctx;

static const de_byte v1pal[1024] = {
	0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0x00, 0x00,0x00,0xff,0x00, 0x00,0xfe,0x00,0x00,
	0xfe,0x00,0x00,0x00, 0x00,0xff,0xff,0x00, 0xff,0x00,0xff,0x00, 0xff,0xff,0x00,0x00,
	0x0f,0x0f,0x0f,0x00, 0x17,0x17,0x17,0x00, 0x1f,0x1f,0x1f,0x00, 0x27,0x27,0x27,0x00,
	0x38,0x38,0x38,0x00, 0x40,0x40,0x40,0x00, 0x48,0x48,0x48,0x00, 0x4f,0x4f,0x4f,0x00,
	0x60,0x60,0x60,0x00, 0x68,0x68,0x68,0x00, 0x70,0x70,0x70,0x00, 0x80,0x80,0x80,0x00,
	0x97,0x97,0x97,0x00, 0xa0,0xa0,0xa0,0x00, 0xb0,0xb0,0xb0,0x00, 0xb8,0xb8,0xb8,0x00,
	0xbf,0xbf,0xbf,0x00, 0xc8,0xc8,0xc8,0x00, 0xd9,0xd9,0xd9,0x00, 0xe0,0xe0,0xe0,0x00,
	0xe8,0xe8,0xe8,0x00, 0xf0,0xf0,0xf0,0x00, 0x00,0x00,0xc0,0x00, 0x06,0x06,0x17,0x00,
	0x06,0x05,0x27,0x00, 0x0b,0x0b,0x40,0x00, 0x0f,0x0f,0x50,0x00, 0x17,0x17,0x68,0x00,
	0x17,0x17,0x80,0x00, 0x1b,0x1b,0x98,0x00, 0x20,0x1f,0xa0,0x00, 0x24,0x23,0xb8,0x00,
	0x28,0x27,0xc8,0x00, 0x2b,0x2b,0xe0,0x00, 0x2f,0x2f,0xf1,0x00, 0x40,0x40,0xff,0x00,
	0x50,0x50,0xfe,0x00, 0x60,0x60,0xff,0x00, 0x6f,0x70,0xff,0x00, 0x7f,0x80,0xff,0x00,
	0x98,0x98,0xff,0x00, 0xa0,0xa0,0xff,0x00, 0xb0,0xb0,0xff,0x00, 0xc0,0xc0,0xfe,0x00,
	0xd0,0xd0,0xff,0x00, 0xe0,0xe0,0xff,0x00, 0xf0,0xf0,0xff,0x00, 0x00,0xc0,0x00,0x00,
	0x05,0x17,0x06,0x00, 0x05,0x27,0x06,0x00, 0x0b,0x40,0x0a,0x00, 0x0f,0x50,0x0f,0x00,
	0x17,0x67,0x17,0x00, 0x17,0x7f,0x17,0x00, 0x1b,0x97,0x1b,0x00, 0x20,0xa0,0x1f,0x00,
	0x24,0xb7,0x24,0x00, 0x27,0xc8,0x27,0x00, 0x2b,0xe0,0x2c,0x00, 0x2f,0xf0,0x30,0x00,
	0x40,0xff,0x40,0x00, 0x50,0xff,0x50,0x00, 0x60,0xff,0x60,0x00, 0x70,0xff,0x70,0x00,
	0x80,0xff,0x80,0x00, 0x98,0xff,0x98,0x00, 0xa0,0xff,0x9f,0x00, 0xb0,0xff,0xb0,0x00,
	0xc0,0xfe,0xc0,0x00, 0xd1,0xfe,0xd0,0x00, 0xe1,0xff,0xe0,0x00, 0xf0,0xff,0xf0,0x00,
	0xc0,0x00,0x00,0x00, 0x17,0x06,0x06,0x00, 0x28,0x06,0x05,0x00, 0x3f,0x0b,0x0a,0x00,
	0x4f,0x0e,0x0f,0x00, 0x68,0x17,0x17,0x00, 0x80,0x17,0x17,0x00, 0x98,0x1b,0x1c,0x00,
	0xa0,0x20,0x20,0x00, 0xb8,0x24,0x23,0x00, 0xc8,0x28,0x27,0x00, 0xe0,0x2b,0x2b,0x00,
	0xf0,0x2f,0x2f,0x00, 0xff,0x40,0x40,0x00, 0xff,0x50,0x50,0x00, 0xff,0x5f,0x60,0x00,
	0xff,0x70,0x6f,0x00, 0xff,0x80,0x80,0x00, 0xfe,0x97,0x97,0x00, 0xff,0xa0,0x9f,0x00,
	0xff,0xaf,0xb0,0x00, 0xff,0xc0,0xc0,0x00, 0xff,0xd0,0xd0,0x00, 0xfe,0xe0,0xe0,0x00,
	0xff,0xf0,0xf0,0x00, 0x00,0xc1,0xc0,0x00, 0x06,0x17,0x17,0x00, 0x05,0x27,0x28,0x00,
	0x0b,0x40,0x40,0x00, 0x0f,0x50,0x4f,0x00, 0x17,0x67,0x68,0x00, 0x17,0x80,0x80,0x00,
	0x1b,0x98,0x97,0x00, 0x1f,0xa0,0xa0,0x00, 0x24,0xb8,0xb8,0x00, 0x27,0xc8,0xc8,0x00,
	0x2b,0xe0,0xe1,0x00, 0x30,0xf0,0xf0,0x00, 0x3f,0xff,0xff,0x00, 0x50,0xff,0xff,0x00,
	0x60,0xff,0xfe,0x00, 0x6f,0xff,0xff,0x00, 0x80,0xff,0xff,0x00, 0x98,0xff,0xff,0x00,
	0xa0,0xfe,0xff,0x00, 0xb1,0xfe,0xfe,0x00, 0xc0,0xff,0xff,0x00, 0xd0,0xff,0xff,0x00,
	0xe0,0xff,0xfe,0x00, 0xf0,0xff,0xff,0x00, 0xc0,0x00,0xc0,0x00, 0x17,0x05,0x17,0x00,
	0x27,0x05,0x27,0x00, 0x3f,0x0a,0x40,0x00, 0x50,0x0f,0x50,0x00, 0x68,0x17,0x68,0x00,
	0x7f,0x17,0x80,0x00, 0x98,0x1b,0x98,0x00, 0x9f,0x1f,0xa0,0x00, 0xb8,0x23,0xb8,0x00,
	0xc8,0x27,0xc9,0x00, 0xe1,0x2b,0xe0,0x00, 0xf1,0x2f,0xf0,0x00, 0xfe,0x40,0xff,0x00,
	0xfe,0x50,0xff,0x00, 0xff,0x5f,0xff,0x00, 0xff,0x70,0xff,0x00, 0xff,0x7f,0xff,0x00,
	0xff,0x98,0xfe,0x00, 0xff,0xa0,0xfe,0x00, 0xff,0xb0,0xff,0x00, 0xff,0xc0,0xff,0x00,
	0xfe,0xcf,0xff,0x00, 0xff,0xdf,0xff,0x00, 0xff,0xf0,0xff,0x00, 0xc0,0xc0,0x00,0x00,
	0x27,0x27,0x06,0x00, 0x40,0x40,0x0a,0x00, 0x50,0x4f,0x0f,0x00, 0x68,0x68,0x18,0x00,
	0x80,0x80,0x17,0x00, 0x98,0x98,0x1b,0x00, 0xa0,0xa0,0x1f,0x00, 0xb8,0xb8,0x23,0x00,
	0xc8,0xc8,0x27,0x00, 0xdf,0xe0,0x2c,0x00, 0xf0,0xf0,0x30,0x00, 0xff,0xff,0x40,0x00,
	0xff,0xff,0x4f,0x00, 0xff,0xff,0x60,0x00, 0xfe,0xff,0x70,0x00, 0xfe,0xff,0x80,0x00,
	0xfe,0xff,0x97,0x00, 0xff,0xff,0xa0,0x00, 0xff,0xff,0xaf,0x00, 0xff,0xff,0xc1,0x00,
	0xff,0xff,0xcf,0x00, 0xff,0xff,0xf1,0x00, 0x05,0x0f,0x17,0x00, 0x05,0x17,0x27,0x00,
	0x0a,0x1f,0x40,0x00, 0x0f,0x27,0x50,0x00, 0x17,0x38,0x67,0x00, 0x17,0x40,0x80,0x00,
	0x1b,0x48,0x98,0x00, 0x1f,0x50,0xa0,0x00, 0x23,0x60,0xb8,0x00, 0x28,0x68,0xc8,0x00,
	0x2b,0x70,0xe0,0x00, 0x2f,0x80,0xf0,0x00, 0x40,0x88,0xf8,0x00, 0x50,0x98,0xf4,0x00,
	0x60,0x97,0xf4,0x00, 0x70,0xa0,0xf8,0x00, 0x80,0xb0,0xf8,0x00, 0x98,0xb8,0xf8,0x00,
	0xa0,0xbe,0xf9,0x00, 0xb1,0xc8,0xfa,0x00, 0xc0,0xd9,0xff,0x00, 0xd0,0xe0,0xff,0x00,
	0xe0,0xe8,0xff,0x00, 0xf0,0xf0,0xff,0x00, 0x0f,0x17,0x28,0x00, 0x17,0x23,0x40,0x00,
	0x1f,0x2f,0x4f,0x00, 0x28,0x40,0x67,0x00, 0x30,0x48,0x7f,0x00, 0x38,0x54,0x98,0x00,
	0x40,0x60,0x9f,0x00, 0x48,0x6c,0xb8,0x00, 0x50,0x80,0xc8,0x00, 0x5c,0x84,0xd9,0x00,
	0x68,0x97,0xe0,0x00, 0x73,0x9c,0xdf,0x00, 0x80,0xa8,0xe4,0x00, 0x97,0xb7,0xe8,0x00,
	0x98,0xc0,0xe9,0x00, 0xa4,0xcc,0xef,0x00, 0xb0,0xd8,0xef,0x00, 0xbc,0xe4,0xf8,0x00,
	0xc8,0xf0,0xf9,0x00, 0xd4,0xf8,0xf8,0x00, 0xe1,0xf4,0xff,0x00, 0xf0,0xf7,0xff,0x00,
	0x7f,0x50,0x50,0x00, 0x88,0x5f,0x5f,0x00, 0x97,0x68,0x68,0x00, 0x98,0x6f,0x70,0x00,
	0xa0,0x80,0x80,0x00, 0xb0,0x88,0x88,0x00, 0xbe,0x98,0x97,0x00, 0xc8,0x98,0x98,0x00,
	0xd9,0xa0,0xa0,0x00, 0xe0,0xb0,0xb0,0x00, 0xe8,0xb8,0xb8,0x00, 0x4f,0x80,0x50,0x00,
	0x60,0x87,0x5f,0x00, 0x68,0x98,0x67,0x00, 0x70,0x98,0x6f,0x00, 0x80,0xa0,0x7f,0x00,
	0x87,0xb0,0x88,0x00, 0x98,0xbe,0x98,0x00, 0x98,0xc8,0x98,0x00, 0x9f,0xd8,0xa0,0x00,
	0xb0,0xe1,0xb0,0x00, 0xb8,0xe8,0xb8,0x00, 0xf0,0x7f,0x00,0x00, 0x80,0xf0,0x00,0x00,
	0xf0,0x00,0x7f,0x00, 0x00,0xf0,0x78,0x00, 0x00,0x80,0xf1,0x00, 0x80,0x00,0xf0,0x00,
	0x37,0x00,0xc1,0x00, 0x80,0x90,0xa8,0x00, 0x48,0x68,0x60,0x00, 0x60,0x78,0x88,0x00
};

static int do_read_header(deark *c, lctx *d, de_int64 pos)
{
	int retval = 0;

	de_dbg(c, "header at %d\n", (int)pos);
	de_dbg_indent(c, 1);

	pos += 15;
	d->ver_major = (unsigned int)de_getui16be(pos);
	d->ver_minor = (unsigned int)de_getui16be(pos+2);
	d->ver_combined = (d->ver_major<<16) | d->ver_minor;
	de_dbg(c, "format version: %u.%u\n", d->ver_major, d->ver_minor);
	pos+=4;

	if(d->ver_major<1 || d->ver_major>2) {
		de_err(c, "Unsupported JBF format version: %u.%u\n", d->ver_major, d->ver_minor);
		goto done;
	}
	if(d->ver_major==1 && (d->ver_minor==2 || d->ver_minor>3)) {
		de_warn(c, "Unrecognized JBF format version (%u.%u). File may not be "
			"decoded correctly.\n", d->ver_major, d->ver_minor);
	}

	d->image_count = de_getui32le(pos);
	de_dbg(c, "image count: %d\n", (int)d->image_count);
	pos+=4;
	if(d->image_count > DE_MAX_IMAGES_PER_FILE) goto done;

	retval = 1;
done:
	de_dbg_indent(c, -1);
	return retval;
}

static const char *get_type_name(unsigned int filetype_code)
{
	const char *nm = "unknown";

	switch(filetype_code) {
	// There are many more PSP file types. These are just some common ones.
	case 0x00: nm="none"; break;
	case 0x01: nm="BMP"; break;
	case 0x0a: nm="GIF"; break;
	case 0x11: nm="JPEG"; break;
	case 0x1c: nm="PNG"; break;
	case 0x1f: nm="PSP"; break;
	case 0x24: nm="TIFF"; break;
	}
	return nm;
}

static int read_filename(deark *c, lctx *d, struct page_ctx *pg, de_int64 pos1, de_int64 *bytes_consumed)
{
	int retval = 0;
	de_int64 pos = pos1;
	de_ucstring *fname_orig = NULL;
	char fn_printable[260];

	fname_orig = ucstring_create(c);

	if(d->ver_combined>=0x010001) { // v1.1+
		de_int64 fnlen;
		fnlen = de_getui32le(pos);
		de_dbg(c, "original filename len: %d\n", (int)fnlen);
		pos += 4;
		if(fnlen>1000) {
			de_err(c, "Bad filename length\n");
			goto done;
		}

		// I don't think there's any way to know the encoding of the filename.
		// WINDOWS1252 is just a guess.
		dbuf_read_to_ucstring(c->infile, pos, fnlen, fname_orig, 0, DE_ENCODING_WINDOWS1252);
		pos += fnlen;
	}
	else { // v1.0
		// File always has 13 bytes reserved for the filename.
		// The name is up to 12 bytes long, terminated by 0x00.
		dbuf_read_to_ucstring(c->infile, pos, 12, fname_orig, DE_CONVFLAG_STOP_AT_NUL, DE_ENCODING_WINDOWS1252);
		pos += 13;
	}

	ucstring_to_printable_sz(fname_orig, fn_printable, sizeof(fn_printable));
	de_dbg(c, "original filename: \"%s\"\n", fn_printable);

	if(c->filenames_from_file) {
		pg->fname = ucstring_clone(fname_orig);
		if(d->ver_major>=2)
			ucstring_append_sz(pg->fname, ".jpg", DE_ENCODING_ASCII);
		else
			ucstring_append_sz(pg->fname, ".bmp", DE_ENCODING_ASCII);
		de_finfo_set_name_from_ucstring(c, pg->fi, pg->fname);
		pg->fi->original_filename_flag = 1;
	}
	else {
		if(d->ver_major>=2)
			pg->thumbnail_ext = "jpg";
		else
			pg->thumbnail_ext = "bmp";
	}

	retval = 1;
done:
	ucstring_destroy(fname_orig);
	*bytes_consumed = pos - pos1;
	return retval;
}

static void read_FILETIME(deark *c, lctx *d, struct page_ctx *pg, de_int64 pos)
{
	de_int64 ft;
	char timestamp_buf[64];

	ft = de_geti64le(pos);
	de_FILETIME_to_timestamp(ft, &pg->fi->mod_time);
	de_timestamp_to_string(&pg->fi->mod_time, timestamp_buf, sizeof(timestamp_buf), 1);
	de_dbg(c, "mod time: %s\n", timestamp_buf);
}

static void read_unix_time(deark *c, lctx *d, struct page_ctx *pg, de_int64 pos)
{
	de_int64 ut;
	char timestamp_buf[64];

	ut = dbuf_geti32le(c->infile, pos);
	de_unix_time_to_timestamp(ut, &pg->fi->mod_time);
	de_timestamp_to_string(&pg->fi->mod_time, timestamp_buf, sizeof(timestamp_buf), 1);
	de_dbg(c, "mod time: %s\n", timestamp_buf);
}

static int read_bitmap_v1(deark *c, lctx *d, struct page_ctx *pg, de_int64 pos1, de_int64 *bytes_consumed)
{
	struct de_bmpinfo bi;
	int retval = 0;
	dbuf *outf = NULL;
	de_int64 pos = pos1;
	de_int64 count;
	de_int64 dec_bytes = 0;

	de_dbg(c, "bitmap at %d\n", (int)pos);
	de_dbg_indent(c, 1);

	if(!de_fmtutil_get_bmpinfo(c, c->infile, &bi, pos, c->infile->len-pos, 0)) {
		de_err(c, "Invalid bitmap\n");
		goto done;
	}

	if(bi.infohdrsize != 40) {
		de_err(c, "Unexpected BMP format\n");
		goto done;
	}

	outf = dbuf_create_output_file(c, pg->thumbnail_ext, pg->fi, 0);
	// Manufacture a BMP fileheader
	dbuf_write(outf, (const de_byte*)"BM", 2);
	dbuf_writeui32le(outf, 14 + bi.total_size);
	dbuf_write_zeroes(outf, 4);
	dbuf_writeui32le(outf, 14 + bi.size_of_headers_and_pal);

	// Copy the BITMAPINFOHEADER
	dbuf_copy(c->infile, pos, bi.infohdrsize, outf);

	// Write the standard palette
	dbuf_write(outf, v1pal, 1024);

	pos += bi.infohdrsize;
	// Decompress the image
	while(1) {
		de_byte b0, b1;

		// Stop if we reach the end of the input file.
		if(pos >= c->infile->len) break;

		// Stop if we decompressed the expected number of bytes
		if(dec_bytes >= bi.foreground_size) break;

		b0 = de_getbyte(pos++);

		if(d->ver_minor>=3) {
			if(b0>0x80) { // a compressed run
				count = (de_int64)(b0-0x80);
				b1 = de_getbyte(pos++);
				dbuf_write_run(outf, b1, count);
				dec_bytes += count;
			}
			else { // uncompressed run
				count = (de_int64)b0;
				dbuf_copy(c->infile, pos, count, outf);
				pos += count;
				dec_bytes += count;
			}
		}
		else {
			if(b0>0xc0) { // a compressed run
				count = (de_int64)(b0-0xc0);
				b1 = de_getbyte(pos++);
				dbuf_write_run(outf, b1, count);
				dec_bytes += count;
			}
			else { // literal byte
				count = 1;
				dbuf_writebyte(outf, b0);
				dec_bytes += 1;
			}
		}
	}

	retval = 1;
done:
	dbuf_close(outf);
	*bytes_consumed = pos - pos1;
	de_dbg_indent(c, -1);
	return retval;
}

static int do_one_thumbnail(deark *c, lctx *d, de_int64 pos1, de_int64 *bytes_consumed)
{
	de_int64 payload_len;
	int retval = 0;
	de_int64 pos = pos1;
	unsigned int filetype_code;
	de_int64 file_size;
	de_int64 x;
	de_int64 tn_w, tn_h;
	struct page_ctx *pg = NULL;
	de_int64 fn_field_size = 0;

	de_dbg(c, "image at %d\n", (int)pos1);
	de_dbg_indent(c, 1);

	pg = de_malloc(c, sizeof(struct page_ctx));

	pg->fi = de_finfo_create(c);

	if(!read_filename(c, d, pg, pos, &fn_field_size)) {
		goto done;
	}
	pos += fn_field_size;

	if(d->ver_major==2) {
		read_FILETIME(c, d, pg, pos);
		pos += 8;
	}

	if(d->ver_major==2) {
		// The original file type (not the format of the thumbnail)
		filetype_code = (unsigned int)de_getui32le(pos);
		de_dbg(c, "original file type: 0x%02x (%s)\n", filetype_code, get_type_name(filetype_code));
		pos += 4; // filetype code
	}
	else if(d->ver_major==1 && d->ver_minor<3) {
		pos += 4; // TODO: FOURCC
	}

	tn_w = de_getui16le(pos);
	pos += 4;
	tn_h = de_getui16le(pos);
	pos += 4;
	de_dbg(c, "original dimensions: %dx%d\n", (int)tn_w, (int)tn_h);

	pos += 4; // color depth

	if(d->ver_major==2) {
		pos += 4; // (uncompressed size?)
	}

	file_size = de_getui32le(pos);
	de_dbg(c, "original file size: %u\n", (unsigned int)file_size);
	pos += 4;

	if(d->ver_major==1) {
		read_unix_time(c, d, pg, pos);
		pos += 4;

		pos += 4; // TODO: image index
	}

	if(d->ver_major==2) {
		// first 4 bytes of 12-byte "thumbnail signature"
		x = de_getui32le(pos);
		pos += 4;
		if(x==0) { // truncated entry
			de_dbg(c, "thumbnail not present\n");
			retval = 1;
			goto done;
		}

		pos += 8; // remaining 8 byte of signature

		payload_len = de_getui32le(pos);
		de_dbg(c, "payload len: %u\n", (unsigned int)payload_len);
		pos += 4;

		if(pos + payload_len > c->infile->len) {
			de_err(c, "Bad payload length (%u) or unsupported format\n", (unsigned int)payload_len);
			goto done;
		}

		dbuf_create_file_from_slice(c->infile, pos, payload_len, pg->thumbnail_ext, pg->fi, 0);
		pos += payload_len;
	}
	else { // ver_major==1
		de_int64 thumbnail_size;
		if(!read_bitmap_v1(c, d, pg, pos, &thumbnail_size)) {
			goto done;
		}
		pos += thumbnail_size;
	}

	retval = 1;
done:
	*bytes_consumed = pos - pos1;
	de_finfo_destroy(c, pg->fi);
	ucstring_destroy(pg->fname);
	de_free(c, pg);
	de_dbg_indent(c, -1);
	return retval;
}

static void de_run_jbf(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;
	de_int64 pos = 0;
	de_int64 bytes_consumed;
	de_int64 count = 0;

	d = de_malloc(c, sizeof(lctx));
	if(!do_read_header(c, d, pos)) goto done;
	pos += 1024;

	count = 0;
	while(1) {
		if(count>=d->image_count) break;
		if(pos>=c->infile->len) goto done;

		bytes_consumed = 0;
		if(!do_one_thumbnail(c, d, pos, &bytes_consumed)) {
			goto done;
		}
		if(bytes_consumed<1) goto done;
		pos += bytes_consumed;
	}

done:
	de_free(c, d);
}

static int de_identify_jbf(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "JASC BROWS FILE", 15))
		return 100;
	return 0;
}

void de_module_jbf(deark *c, struct deark_module_info *mi)
{
	mi->id = "jbf";
	mi->desc = "PaintShop Pro Browser Cache (pspbrwse.jbf)";
	mi->run_fn = de_run_jbf;
	mi->identify_fn = de_identify_jbf;
}
