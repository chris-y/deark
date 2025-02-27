// This file is part of Deark.
// Copyright (C) 2016-2019 Jason Summers
// See the file COPYING for terms of use.

// PNG encoding

#define DE_NOT_IN_MODULE
#include "deark-config.h"
#include "deark-private.h"
#include "deark-fmtutil.h"

#define DE_MAX_IDAT_CHUNKSIZE 1048576

// TODO: Finish removing the "mz" symbols, and other miniz things.
#define MY_MZ_MIN(a,b) (((a)<(b))?(a):(b))
#define MY_TDEFL_WRITE_ZLIB_HEADER  0x01000

#define CODE_IDAT 0x49444154U
#define CODE_IEND 0x49454e44U
#define CODE_IHDR 0x49484452U
#define CODE_htSP 0x68745350U
#define CODE_pHYs 0x70485973U
#define CODE_tEXt 0x74455874U
#define CODE_tIME 0x74494d45U

struct deark_png_encode_info {
	deark *c;
	dbuf *outf;
	de_bitmap *img;
	int width, height;
	int src_rowspan;
	int num_chans;
	int flip;
	unsigned int level;
	int has_phys;
	u32 xdens;
	u32 ydens;
	u8 phys_units;
	struct de_timestamp internal_mod_time;
	u8 include_text_chunk_software;
	u8 has_hotspot;
	u8 encode_as_bwimg;
	int hotspot_x, hotspot_y;
	struct de_crcobj *crco;
	size_t dst_rowspan;
	u8 *tmprow;
};

static void write_png_chunk_from_mem(struct deark_png_encode_info *pei,
	const u8 *mem, i64 memlen, u32 chunktype)
{
	u32 crc;
	u8 tmpbuf[4];

	de_crcobj_reset(pei->crco);

	// length field
	dbuf_writeu32be(pei->outf, memlen);

	// chunk type field
	de_writeu32be_direct(tmpbuf, (i64)chunktype);
	de_crcobj_addbuf(pei->crco, tmpbuf, 4);
	dbuf_write(pei->outf, tmpbuf, 4);

	// data field
	de_crcobj_addbuf(pei->crco, mem, memlen);
	dbuf_write(pei->outf, mem, memlen);

	// CRC field
	crc = de_crcobj_getval(pei->crco);
	dbuf_writeu32be(pei->outf, (i64)crc);
}

static void write_png_chunk_from_cdbuf(struct deark_png_encode_info *pei,
	dbuf *cdbuf, u32 chunktype)
{
	const u8 *mem;

	mem = dbuf_get_membuf_direct_ptr(cdbuf);
	if(mem==NULL && cdbuf->len!=0) goto done;
	write_png_chunk_from_mem(pei, mem, cdbuf->len, chunktype);

done:
	;
}

static void write_png_chunk_IHDR(struct deark_png_encode_info *pei,
	dbuf *cdbuf)
{
	static const u8 color_type_code[] = {0x00, 0x00, 0x04, 0x02, 0x06};

	dbuf_writeu32be(cdbuf, (i64)pei->width);
	dbuf_writeu32be(cdbuf, (i64)pei->height);
	if(pei->encode_as_bwimg) {
		dbuf_writebyte(cdbuf, 1); // bit depth
		dbuf_writebyte(cdbuf, 0x00);
	}
	else {
		dbuf_writebyte(cdbuf, 8); // bit depth
		dbuf_writebyte(cdbuf, color_type_code[pei->num_chans]);
	}
	dbuf_truncate(cdbuf, 13); // rest of chunk is zeroes
	write_png_chunk_from_cdbuf(pei, cdbuf, CODE_IHDR);
}

static void write_png_chunk_pHYs(struct deark_png_encode_info *pei,
	dbuf *cdbuf)
{
	dbuf_writeu32be(cdbuf, (i64)pei->xdens);
	dbuf_writeu32be(cdbuf, (i64)pei->ydens);
	dbuf_writebyte(cdbuf, pei->phys_units);
	write_png_chunk_from_cdbuf(pei, cdbuf, CODE_pHYs);
}

static void write_png_chunk_tIME(struct deark_png_encode_info *pei,
	dbuf *cdbuf)
{
	struct de_struct_tm tm2;

	de_gmtime(&pei->internal_mod_time, &tm2);
	if(!tm2.is_valid) return;

	dbuf_writeu16be(cdbuf, (i64)tm2.tm_fullyear);
	dbuf_writebyte(cdbuf, (u8)(1+tm2.tm_mon));
	dbuf_writebyte(cdbuf, (u8)tm2.tm_mday);
	dbuf_writebyte(cdbuf, (u8)tm2.tm_hour);
	dbuf_writebyte(cdbuf, (u8)tm2.tm_min);
	dbuf_writebyte(cdbuf, (u8)tm2.tm_sec);
	write_png_chunk_from_cdbuf(pei, cdbuf, CODE_tIME);
}

static void write_png_chunk_htSP(struct deark_png_encode_info *pei,
	dbuf *cdbuf)
{
	// This is the UUID b9fe4f3d-8f32-456f-aa02-dcd79cce0e24
	static const u8 uuid[16] = {0xb9,0xfe,0x4f,0x3d,0x8f,0x32,0x45,0x6f,
		0xaa,0x02,0xdc,0xd7,0x9c,0xce,0x0e,0x24};

	dbuf_write(cdbuf, uuid, 16);
	dbuf_writei32be(cdbuf, (i64)pei->hotspot_x);
	dbuf_writei32be(cdbuf, (i64)pei->hotspot_y);
	write_png_chunk_from_cdbuf(pei, cdbuf, CODE_htSP);
}

static void write_png_chunk_tEXt(struct deark_png_encode_info *pei,
	dbuf *cdbuf, const char *keyword, const char *value)
{
	i64 kwlen, vlen;
	kwlen = (i64)de_strlen(keyword);
	vlen = (i64)de_strlen(value);
	dbuf_write(cdbuf, (const u8*)keyword, kwlen);
	dbuf_writebyte(cdbuf, 0);
	dbuf_write(cdbuf, (const u8*)value, vlen);
	write_png_chunk_from_cdbuf(pei, cdbuf, CODE_tEXt);
}

struct IDAT_write_userdata_struct {
	struct deark_png_encode_info *pei;
	dbuf *cdbuf;
	int IDAT_count;
};

static void my_IDAT_write_cb(dbuf *f, void *userdata,
	const u8 *mem_orig, i64 memsize_orig)
{
	struct IDAT_write_userdata_struct *iwu = (struct IDAT_write_userdata_struct*)userdata;
	const u8 *mem = mem_orig;
	i64 memsize = memsize_orig;

	// We *could* just write a single IDAT chunk for each call to this function.
	// We would get a PNG with IDATs having various random-ish sizes.
	// I want the PNG output to be more predictable and stable than that, though,
	// so we'll do some buffering to make each chunk (except the last) have
	// DE_MAX_IDAT_CHUNKSIZE bytes.

	while(iwu->cdbuf->len + memsize >= DE_MAX_IDAT_CHUNKSIZE) {
		i64 amt_to_use_from_membuf;

		if(iwu->cdbuf->len >= DE_MAX_IDAT_CHUNKSIZE) {
			amt_to_use_from_membuf = 0;
		}
		else {
			amt_to_use_from_membuf = DE_MAX_IDAT_CHUNKSIZE - iwu->cdbuf->len;
		}

		dbuf_write(iwu->cdbuf, mem, amt_to_use_from_membuf);
		mem += amt_to_use_from_membuf;
		memsize -= amt_to_use_from_membuf;
		// cdbuf should now have exactly DE_MAX_IDAT_CHUNKSIZE bytes in it.

		write_png_chunk_from_cdbuf(iwu->pei, iwu->cdbuf, CODE_IDAT);
		iwu->IDAT_count++;
		dbuf_empty(iwu->cdbuf);
	}

	// Save any remaining compressed pixel data for next time.
	if(memsize > 0) {
		dbuf_write(iwu->cdbuf, mem, memsize);
	}
}

static void compress_png_row(struct deark_png_encode_info *pei, struct fmtutil_tdefl_ctx *tdctx,
	int y)
{
	static const char nulbyte = '\0';

	// Filter byte
	fmtutil_tdefl_compress_buffer(tdctx, &nulbyte, 1, FMTUTIL_TDEFL_NO_FLUSH);

	if(pei->encode_as_bwimg) {
		int x;

		if(pei->tmprow) {
			de_zeromem(pei->tmprow, pei->dst_rowspan);
		}
		else {
			pei->tmprow = de_malloc(pei->c, pei->dst_rowspan);
		}

		for(x=0; x<pei->width; x++) {
			u8 k;

			// We just use the red sample, like DE_COLOR_K().
			k = pei->img->bitmap[y*pei->src_rowspan + x*pei->img->bytes_per_pixel];
			if(k>=0x80) {
				pei->tmprow[x/8] |= 1U<<(7-x%8);
			}
		}

		fmtutil_tdefl_compress_buffer(tdctx, pei->tmprow, pei->dst_rowspan, FMTUTIL_TDEFL_NO_FLUSH);
	}
	else {
		fmtutil_tdefl_compress_buffer(tdctx, &pei->img->bitmap[y*pei->src_rowspan],
			pei->dst_rowspan, FMTUTIL_TDEFL_NO_FLUSH);
	}
}

static int write_png_chunk_IDATs(struct deark_png_encode_info *pei, dbuf *cdbuf)
{
	int y;
	int retval = 0;
	deark *c = pei->c;
	struct fmtutil_tdefl_ctx *tdctx = NULL;
	static const unsigned int my_s_tdefl_num_probes[11] = { 0, 1, 6, 32,  16, 32, 128, 256,  512, 768, 1500 };
	dbuf *outf_IDAT = NULL;
	struct IDAT_write_userdata_struct iwu;

	de_zeromem(&iwu, sizeof(struct IDAT_write_userdata_struct));
	iwu.pei = pei;
	iwu.cdbuf = cdbuf;

	outf_IDAT = dbuf_create_custom_dbuf(c, 0, 0);
	outf_IDAT->userdata_for_customwrite = (void*)&iwu;
	outf_IDAT->customwrite_fn = my_IDAT_write_cb;

	// compress image data
	tdctx = fmtutil_tdefl_create(c, outf_IDAT,
		my_s_tdefl_num_probes[MY_MZ_MIN(10, pei->level)] | MY_TDEFL_WRITE_ZLIB_HEADER);

	if(pei->encode_as_bwimg) {
		pei->dst_rowspan = ((size_t)pei->width+7)/8;
	}
	else {
		pei->dst_rowspan = (size_t)pei->width * (size_t)pei->num_chans;
	}

	for (y = 0; y<pei->height; y++) {
		compress_png_row(pei, tdctx, (pei->flip ? (pei->height - 1 - y) : y));
	}

	if (fmtutil_tdefl_compress_buffer(tdctx, NULL, 0, FMTUTIL_TDEFL_FINISH) !=
		FMTUTIL_TDEFL_STATUS_DONE)
	{
		goto done;
	}

	if(cdbuf->len>0 || iwu.IDAT_count==0) {
		write_png_chunk_from_cdbuf(pei, cdbuf, CODE_IDAT);
	}

	retval = 1;

done:
	fmtutil_tdefl_destroy(tdctx);
	dbuf_close(outf_IDAT);
	return retval;
}

static int do_generate_png(struct deark_png_encode_info *pei, const u8 *src_pixels)
{
	static const u8 pngsig[8] = { 0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a };
	dbuf *cdbuf = NULL;
	int retval = 0;

	// A membuf that we'll use and reuse for each chunk's data
	cdbuf = dbuf_create_membuf(pei->c, 64, 0);

	dbuf_write(pei->outf, pngsig, 8);

	write_png_chunk_IHDR(pei, cdbuf);

	if(pei->has_phys) {
		dbuf_truncate(cdbuf, 0);
		write_png_chunk_pHYs(pei, cdbuf);
	}

	if(pei->internal_mod_time.is_valid && pei->c->preserve_file_times_internal) {
		dbuf_truncate(cdbuf, 0);
		write_png_chunk_tIME(pei, cdbuf);
	}

	if(pei->has_hotspot) {
		dbuf_truncate(cdbuf, 0);
		write_png_chunk_htSP(pei, cdbuf);
	}

	if(pei->include_text_chunk_software) {
		dbuf_truncate(cdbuf, 0);
		write_png_chunk_tEXt(pei, cdbuf, "Software", "Deark");
	}

	dbuf_truncate(cdbuf, 0);
	if(!write_png_chunk_IDATs(pei, cdbuf)) goto done;

	dbuf_truncate(cdbuf, 0);
	write_png_chunk_from_cdbuf(pei, cdbuf, CODE_IEND);
	retval = 1;

done:
	dbuf_close(cdbuf);
	return retval;
}

// flags2:
//   0x1 = image can be encoded as bi-level, black&white, opaque
int de_write_png(deark *c, de_bitmap *img, dbuf *f, UI createflags, UI flags2)
{
	const char *opt_level;
	int retval = 0;
	struct deark_png_encode_info *pei = NULL;

	pei = de_malloc(c, sizeof(struct deark_png_encode_info));
	pei->c = c;

	if(img->invalid_image_flag) {
		goto done;
	}
	if(!de_good_image_dimensions(c, img->width, img->height)) {
		goto done;
	}

	// Optimization to speed up list mode
	if(f->btype==DBUF_TYPE_NULL && !c->enable_oinfo) {
		goto done;
	}

	pei->img = img;
	if(flags2 & 0x1) {
		pei->encode_as_bwimg = 1;
	}

	if(f->fi_copy && f->fi_copy->density.code>0 && c->write_density) {
		pei->has_phys = 1;
		if(f->fi_copy->density.code==1) { // unspecified units
			pei->phys_units = 0;
			pei->xdens = (u32)(f->fi_copy->density.xdens+0.5);
			pei->ydens = (u32)(f->fi_copy->density.ydens+0.5);
		}
		else if(f->fi_copy->density.code==2) { // dpi
			pei->phys_units = 1; // pixels/meter
			pei->xdens = (u32)(0.5+f->fi_copy->density.xdens/0.0254);
			pei->ydens = (u32)(0.5+f->fi_copy->density.ydens/0.0254);
		}
	}

	if(pei->has_phys && pei->xdens==pei->ydens && pei->phys_units==0) {
		// Useless density information. Don't bother to write it.
		pei->has_phys = 0;
	}

	// Detect likely-bogus density settings.
	if(pei->has_phys) {
		if(pei->xdens<=0 || pei->ydens<=0 ||
			(pei->xdens > pei->ydens*5) || (pei->ydens > pei->xdens*5))
		{
			pei->has_phys = 0;
		}
	}

	pei->outf = f;
	if(!c->padpix && img->unpadded_width>0 && img->unpadded_width<img->width) {
		pei->width = (int)img->unpadded_width;
	}
	else {
		pei->width = (int)img->width;
	}
	pei->src_rowspan = (int)(img->width * img->bytes_per_pixel);
	pei->height = (int)img->height;
	pei->flip = (createflags & DE_CREATEFLAG_FLIP_IMAGE)?1:0;
	pei->num_chans = img->bytes_per_pixel;
	pei->include_text_chunk_software = 0;

	if(!c->pngcprlevel_valid) {
		c->pngcmprlevel = 9; // default
		c->pngcprlevel_valid = 1;

		opt_level = de_get_ext_option(c, "pngcmprlevel");
		if(opt_level) {
			i64 opt_level_n = de_atoi64(opt_level);
			if(opt_level_n>10) {
				c->pngcmprlevel = 10;
			}
			else if(opt_level_n<0) {
				c->pngcmprlevel = 6;
			}
			else {
				c->pngcmprlevel = (unsigned int)opt_level_n;
			}
		}
	}
	pei->level = c->pngcmprlevel;

	if(f->fi_copy && f->fi_copy->internal_mod_time.is_valid) {
		pei->internal_mod_time = f->fi_copy->internal_mod_time;
	}

	if(f->fi_copy && f->fi_copy->has_hotspot) {
		pei->has_hotspot = 1;
		pei->hotspot_x = f->fi_copy->hotspot_x;
		pei->hotspot_y = f->fi_copy->hotspot_y;
		// Leave a hint as to where our custom Hotspot chunk came from.
		pei->include_text_chunk_software = 1;
	}

	pei->crco = de_crcobj_create(c, DE_CRCOBJ_CRC32_IEEE);

	if(!do_generate_png(pei, img->bitmap)) {
		de_err(c, "PNG write failed");
		goto done;
	}

	retval = 1;

done:
	if(pei) {
		de_crcobj_destroy(pei->crco);
		de_free(c, pei->tmprow);
		de_free(c, pei);
	}
	return retval;
}
