// This file is part of Deark.
// Copyright (C) 2018 Jason Summers
// See the file COPYING for terms of use.

// Decompression, etc.

#define DE_NOT_IN_MODULE
#include "deark-config.h"
#include "deark-private.h"
#include "deark-fmtutil.h"

// Returns a message that is valid until the next operation on dres.
const char *de_dfilter_get_errmsg(deark *c, struct de_dfilter_results *dres)
{
	if(dres->errcode==0) {
		return "No error";
	}
	if(dres->errmsg[0]) {
		return dres->errmsg;
	}
	return "Unspecified error";
}

// Initialize or reset a dfilter results struct
void de_dfilter_results_clear(deark *c, struct de_dfilter_results *dres)
{
	dres->errcode = 0;
	dres->bytes_consumed_valid = 0;
	dres->bytes_consumed = 0;
	dres->errmsg[0] = '\0';
}

// Note: It is also okay to init these objects by zeroing out their bytes.
void de_dfilter_init_objects(deark *c, struct de_dfilter_in_params *dcmpri,
	struct de_dfilter_out_params *dcmpro, struct de_dfilter_results *dres)
{
	if(dcmpri)
		de_zeromem(dcmpri, sizeof(struct de_dfilter_in_params));
	if(dcmpro)
		de_zeromem(dcmpro, sizeof(struct de_dfilter_out_params));
	if(dres)
		de_dfilter_results_clear(c, dres);
}

void de_dfilter_set_errorf(deark *c, struct de_dfilter_results *dres, const char *modname,
	const char *fmt, ...)
{
	va_list ap;

	if(dres->errcode != 0) return; // Only record the first error
	dres->errcode = 1;

	va_start(ap, fmt);
	if(modname) {
		char tmpbuf[80];

		de_vsnprintf(tmpbuf, sizeof(tmpbuf), fmt, ap);
		de_snprintf(dres->errmsg, sizeof(dres->errmsg), "[%s] %s", modname, tmpbuf);
	}
	else {
		de_vsnprintf(dres->errmsg, sizeof(dres->errmsg), fmt, ap);
	}
	va_end(ap);
}

void de_dfilter_set_generic_error(deark *c, struct de_dfilter_results *dres, const char *modname)
{
	if(dres->errcode != 0) return;
	de_dfilter_set_errorf(c, dres, modname, "Unspecified error");
}

// This is a decompression API that uses a "push" input model. The client
// sends data to the codec as the data becomes available.
// (The client must still be able to consume any amount of output data
// immediately.)
// This model makes it easier to chain multiple codecs together, and to handle
// input data that is not contiguous.
// TODO: There's no reason this couldn't be extended to work with "type1" codecs.

struct de_dfilter_ctx *de_dfilter_create(deark *c,
	dfilter_codec_type codec_init_fn, void *codec_private_params,
	struct de_dfilter_out_params *dcmpro, struct de_dfilter_results *dres)
{
	struct de_dfilter_ctx *dfctx = NULL;

	dfctx = de_malloc(c, sizeof(struct de_dfilter_ctx));
	dfctx->c = c;
	dfctx->dres = dres;
	dfctx->dcmpro = dcmpro;

	if(codec_init_fn) {
		codec_init_fn(dfctx, codec_private_params);
	}
	// TODO: How should we handle failure to initialize a codec?

	return dfctx;
}

void de_dfilter_addbuf(struct de_dfilter_ctx *dfctx,
	const u8 *buf, i64 buf_len)
{
	if(dfctx->finished_flag) return;

	if(dfctx->codec_addbuf_fn && (buf_len>0)) {
		dfctx->codec_addbuf_fn(dfctx, buf, buf_len);

		if(dfctx->dres->errcode) {
			dfctx->finished_flag = 1;
		}
	}
}

// Commands:  (Commands are not supported by all codecs)
//   DE_DFILTER_COMMAND_SOFTRESET
//    Reset the decompressor state. Exact function depends on the codec.
//
//   DE_DFILTER_COMMAND_REINITIALIZE
//    Reinitialize a codec, so you don't have to destroy and recreate it in
//    in order to use it again. Typically used after _finish().
//    Before using this command, it is okay to change the internal parameters of
//    the dcmpro and dres given to de_dfilter_create(). You should call
//    de_dfilter_results_clear or the equivalent if you have already handled
//    previous errors.
void de_dfilter_command(struct de_dfilter_ctx *dfctx, int cmd, UI flags)
{
	// Non-codec-specific things:

	if(cmd==DE_DFILTER_COMMAND_REINITIALIZE) {
		dfctx->finished_flag = 0;
		dfctx->dres->bytes_consumed_valid = 0;
	}

	// Codec-specific things:

	if(dfctx->codec_command_fn) {
		dfctx->codec_command_fn(dfctx, cmd, flags);
	}
}

// Call this to inform the codec that there are no more compressed bytes.
// The codec's 'finish' function should flush any pending output,
// and update the decompression results in dfctx->dres.
// Some codecs can still be used after this, provided you then call
// de_dfilter_command(...,DE_DFILTER_COMMAND_REINITIALIZE).
void de_dfilter_finish(struct de_dfilter_ctx *dfctx)
{
	if(dfctx->codec_finish_fn) {
		dfctx->codec_finish_fn(dfctx);
	}
}

void de_dfilter_destroy(struct de_dfilter_ctx *dfctx)
{
	deark *c;

	if(!dfctx) return;
	c = dfctx->c;
	if(dfctx->codec_destroy_fn) {
		dfctx->codec_destroy_fn(dfctx);
	}

	de_free(c, dfctx);
}

static int my_dfilter_addslice_buffered_read_cbfn(struct de_bufferedreadctx *brctx, const u8 *buf,
	i64 buf_len)
{
	struct de_dfilter_ctx *dfctx = (struct de_dfilter_ctx *)brctx->userdata;

	de_dfilter_addbuf(dfctx, buf, buf_len);
	if(dfctx->finished_flag) return 0;
	return 1;
}

void de_dfilter_addslice(struct de_dfilter_ctx *dfctx,
	dbuf *inf, i64 pos, i64 len)
{
	if(dfctx->finished_flag) return;
	dbuf_buffered_read(inf, pos, len,
		my_dfilter_addslice_buffered_read_cbfn, (void*)dfctx);
}

// Use a "pushable" codec in a non-pushable way.
void de_dfilter_decompress_oneshot(deark *c,
	dfilter_codec_type codec_init_fn, void *codec_private_params,
	struct de_dfilter_in_params *dcmpri, struct de_dfilter_out_params *dcmpro,
	struct de_dfilter_results *dres)
{
	struct de_dfilter_ctx *dfctx = NULL;

	dfctx = de_dfilter_create(c, codec_init_fn, codec_private_params,
		dcmpro, dres);
	dfctx->input_file_offset = dcmpri->pos;
	de_dfilter_addslice(dfctx, dcmpri->f, dcmpri->pos, dcmpri->len);
	de_dfilter_finish(dfctx);
	de_dfilter_destroy(dfctx);
}

// Trivial "decompression" of uncompressed data.
void fmtutil_decompress_uncompressed(deark *c, struct de_dfilter_in_params *dcmpri,
	struct de_dfilter_out_params *dcmpro, struct de_dfilter_results *dres, UI flags)
{
	i64 len;
	i64 nbytes_avail;

	nbytes_avail = de_min_int(dcmpri->len, dcmpri->f->len - dcmpri->pos);

	if(dcmpro->len_known) {
		len = dcmpro->expected_len;
	}
	else {
		len = dcmpri->len;
	}

	if(len>nbytes_avail) len = nbytes_avail;
	if(len<0) len = 0;

	dbuf_copy(dcmpri->f, dcmpri->pos, len, dcmpro->f);
	dres->bytes_consumed = len;
	dres->bytes_consumed_valid = 1;
}

enum packbits_state_enum {
	PACKBITS_STATE_NEUTRAL = 0,
	PACKBITS_STATE_COPYING_LITERAL,
	PACKBITS_STATE_READING_UNIT_TO_REPEAT
};

struct packbitsctx {
	size_t nbytes_per_unit;
	size_t nbytes_in_unitbuf;
	u8 unitbuf[2];
	i64 total_nbytes_processed;
	i64 nbytes_written;
	enum packbits_state_enum state;
	i64 nliteral_bytes_remaining;
	i64 repeat_count;
};

static void my_packbits_codec_addbuf(struct de_dfilter_ctx *dfctx,
	const u8 *buf, i64 buf_len)
{
	int i;
	u8 b;
	struct packbitsctx *rctx = (struct packbitsctx*)dfctx->codec_private;

	if(!rctx) return;

	for(i=0; i<buf_len; i++) {
		if(dfctx->dcmpro->len_known &&
			(rctx->nbytes_written >= dfctx->dcmpro->expected_len))
		{
			dfctx->finished_flag = 1;
			break;
		}

		b = buf[i];
		rctx->total_nbytes_processed++;

		switch(rctx->state) {
		case PACKBITS_STATE_NEUTRAL: // this is a code byte
			if(b>128) { // A compressed run
				rctx->repeat_count = 257 - (i64)b;
				rctx->state = PACKBITS_STATE_READING_UNIT_TO_REPEAT;
			}
			else if(b<128) { // An uncompressed run
				rctx->nliteral_bytes_remaining = (1 + (i64)b) * (i64)rctx->nbytes_per_unit;
				rctx->state = PACKBITS_STATE_COPYING_LITERAL;
			}
			// Else b==128. No-op.
			// TODO: Some (but not most) ILBM specs say that code 128 is used to
			// mark the end of compressed data, so maybe there should be options to
			// tell us what to do when code 128 is encountered.
			break;
		case PACKBITS_STATE_COPYING_LITERAL: // This byte is uncompressed
			dbuf_writebyte(dfctx->dcmpro->f, b);
			rctx->nbytes_written++;
			rctx->nliteral_bytes_remaining--;
			if(rctx->nliteral_bytes_remaining<=0) {
				rctx->state = PACKBITS_STATE_NEUTRAL;
			}
			break;
		case PACKBITS_STATE_READING_UNIT_TO_REPEAT:
			if(rctx->nbytes_per_unit==1) { // Optimization for standard PackBits
				dbuf_write_run(dfctx->dcmpro->f, b, rctx->repeat_count);
				rctx->nbytes_written += rctx->repeat_count;
				rctx->state = PACKBITS_STATE_NEUTRAL;
			}
			else {
				rctx->unitbuf[rctx->nbytes_in_unitbuf++] = b;
				if(rctx->nbytes_in_unitbuf >= rctx->nbytes_per_unit) {
					i64 k;

					for(k=0; k<rctx->repeat_count; k++) {
						dbuf_write(dfctx->dcmpro->f, rctx->unitbuf, (i64)rctx->nbytes_per_unit);
					}
					rctx->nbytes_in_unitbuf = 0;
					rctx->nbytes_written += rctx->repeat_count * (i64)rctx->nbytes_per_unit;
					rctx->state = PACKBITS_STATE_NEUTRAL;
				}
			}
			break;
		}
	}
}

static void my_packbits_codec_command(struct de_dfilter_ctx *dfctx, int cmd, UI flags)
{
	struct packbitsctx *rctx = (struct packbitsctx*)dfctx->codec_private;

	if(cmd==DE_DFILTER_COMMAND_SOFTRESET || cmd==DE_DFILTER_COMMAND_REINITIALIZE) {
		// "soft reset" - reset the low-level compression state, but don't update
		// dres, or the total-bytes counters, etc.
		rctx->state = PACKBITS_STATE_NEUTRAL;
		rctx->nbytes_in_unitbuf = 0;
		rctx->nliteral_bytes_remaining = 0;
		rctx->repeat_count = 0;
	}
	if(cmd==DE_DFILTER_COMMAND_REINITIALIZE) {
		rctx->total_nbytes_processed = 0;
		rctx->nbytes_written = 0;
	}
}

static void my_packbits_codec_finish(struct de_dfilter_ctx *dfctx)
{
	struct packbitsctx *rctx = (struct packbitsctx*)dfctx->codec_private;

	if(!rctx) return;
	dfctx->dres->bytes_consumed = rctx->total_nbytes_processed;
	dfctx->dres->bytes_consumed_valid = 1;
}

static void my_packbits_codec_destroy(struct de_dfilter_ctx *dfctx)
{
	struct packbitsctx *rctx = (struct packbitsctx*)dfctx->codec_private;

	if(rctx) {
		de_free(dfctx->c, rctx);
	}
	dfctx->codec_private = NULL;
}

// codec_private_params: de_packbits_params, or NULL for default params.
void dfilter_packbits_codec(struct de_dfilter_ctx *dfctx, void *codec_private_params)
{
	struct packbitsctx *rctx = NULL;
	struct de_packbits_params *pbparams = (struct de_packbits_params*)codec_private_params;

	rctx = de_malloc(dfctx->c, sizeof(struct packbitsctx));
	rctx->nbytes_per_unit = 1;
	if(pbparams && pbparams->is_packbits16) {
		rctx->nbytes_per_unit = 2;
	}
	dfctx->codec_private = (void*)rctx;
	dfctx->codec_addbuf_fn = my_packbits_codec_addbuf;
	dfctx->codec_finish_fn = my_packbits_codec_finish;
	dfctx->codec_command_fn = my_packbits_codec_command;
	dfctx->codec_destroy_fn = my_packbits_codec_destroy;
}

void fmtutil_decompress_packbits_ex(deark *c, struct de_dfilter_in_params *dcmpri,
	struct de_dfilter_out_params *dcmpro, struct de_dfilter_results *dres,
	struct de_packbits_params *pbparams)
{
	de_dfilter_decompress_oneshot(c, dfilter_packbits_codec, (void*)pbparams,
		dcmpri, dcmpro, dres);
}

// Returns 0 on failure (currently impossible).
int fmtutil_decompress_packbits(dbuf *f, i64 pos1, i64 len,
	dbuf *unc_pixels, i64 *cmpr_bytes_consumed)
{
	struct de_dfilter_results dres;
	struct de_dfilter_in_params dcmpri;
	struct de_dfilter_out_params dcmpro;

	if(cmpr_bytes_consumed) *cmpr_bytes_consumed = 0;
	de_dfilter_init_objects(f->c, &dcmpri, &dcmpro, &dres);

	dcmpri.f = f;
	dcmpri.pos = pos1;
	dcmpri.len = len;
	dcmpro.f = unc_pixels;
	if(unc_pixels->has_len_limit) {
		dcmpro.len_known = 1;
		dbuf_flush(unc_pixels);
		dcmpro.expected_len = unc_pixels->len_limit - unc_pixels->len;
	}

	de_dfilter_decompress_oneshot(f->c, dfilter_packbits_codec, NULL,
		&dcmpri, &dcmpro, &dres);

	if(cmpr_bytes_consumed && dres.bytes_consumed_valid) {
		*cmpr_bytes_consumed = dres.bytes_consumed;
	}
	if(dres.errcode != 0) return 0;
	return 1;
}

void fmtutil_decompress_rle90_ex(deark *c, struct de_dfilter_in_params *dcmpri,
	struct de_dfilter_out_params *dcmpro, struct de_dfilter_results *dres,
	unsigned int flags)
{
	de_dfilter_decompress_oneshot(c, dfilter_rle90_codec, NULL,
		dcmpri, dcmpro, dres);
}

struct rle90ctx {
	i64 total_nbytes_processed;
	i64 nbytes_written;
	u8 last_output_byte;
	int countcode_pending;
};

static void my_rle90_codec_addbuf(struct de_dfilter_ctx *dfctx,
	const u8 *buf, i64 buf_len)
{
	int i;
	u8 b;
	struct rle90ctx *rctx = (struct rle90ctx*)dfctx->codec_private;

	if(!rctx) return;

	for(i=0; i<buf_len; i++) {
		if(dfctx->dcmpro->len_known &&
			(rctx->nbytes_written >= dfctx->dcmpro->expected_len))
		{
			dfctx->finished_flag = 1;
			break;
		}

		b = buf[i];
		rctx->total_nbytes_processed++;

		if(rctx->countcode_pending && b==0) {
			// Not RLE, just an escaped 0x90 byte.
			dbuf_writebyte(dfctx->dcmpro->f, 0x90);
			rctx->nbytes_written++;
			rctx->last_output_byte = 0x90;
			rctx->countcode_pending = 0;
		}
		else if(rctx->countcode_pending) {
			i64 count;

			// RLE. We already emitted one byte (because the byte to repeat
			// comes before the repeat count), so write countcode-1 bytes.
			count = (i64)(b-1);
			if(dfctx->dcmpro->len_known &&
				(rctx->nbytes_written+count > dfctx->dcmpro->expected_len))
			{
				count = dfctx->dcmpro->expected_len - rctx->nbytes_written;
			}
			dbuf_write_run(dfctx->dcmpro->f, rctx->last_output_byte, count);
			rctx->nbytes_written += count;

			rctx->countcode_pending = 0;
		}
		else if(b==0x90) {
			rctx->countcode_pending = 1;
		}
		else {
			dbuf_writebyte(dfctx->dcmpro->f, b);
			rctx->nbytes_written++;
			rctx->last_output_byte = b;
		}
	}
}

static void my_rle90_codec_finish(struct de_dfilter_ctx *dfctx)
{
	struct rle90ctx *rctx = (struct rle90ctx*)dfctx->codec_private;

	if(!rctx) return;
	dfctx->dres->bytes_consumed = rctx->total_nbytes_processed;
	dfctx->dres->bytes_consumed_valid = 1;
}

static void my_rle90_codec_destroy(struct de_dfilter_ctx *dfctx)
{
	struct rle90ctx *rctx = (struct rle90ctx*)dfctx->codec_private;

	if(rctx) {
		de_free(dfctx->c, rctx);
	}
	dfctx->codec_private = NULL;
}

// RLE algorithm occasionally called "RLE90". Variants of this are used by
// BinHex, ARC, StuffIt, and others.
// codec_private_params: Unused, must be NULL.
void dfilter_rle90_codec(struct de_dfilter_ctx *dfctx, void *codec_private_params)
{
	struct rle90ctx *rctx = NULL;

	rctx = de_malloc(dfctx->c, sizeof(struct rle90ctx));
	dfctx->codec_private = (void*)rctx;
	dfctx->codec_addbuf_fn = my_rle90_codec_addbuf;
	dfctx->codec_finish_fn = my_rle90_codec_finish;
	dfctx->codec_destroy_fn = my_rle90_codec_destroy;
}

//======================= lzss1 =======================

// Used by lzss1 & hlp_lz77
struct lzss_ctx {
	i64 nbytes_written;
	int stop_flag;
	struct de_dfilter_in_params *dcmpri;
	struct de_dfilter_out_params *dcmpro;
	struct de_lz77buffer *ringbuf;
	i64 cur_ipos;
	i64 endpos;
	struct de_bitbuf_lowlevel bbll;
};

// Used by lzss1 & hlp_lz77
static void lzss_lz77buf_writebytecb(struct de_lz77buffer *rb, const u8 n)
{
	struct lzss_ctx *sctx = (struct lzss_ctx*)rb->userdata;

	if(sctx->stop_flag) return;
	if(sctx->dcmpro->len_known) {
		if(sctx->nbytes_written >= sctx->dcmpro->expected_len) {
			sctx->stop_flag = 1;
			return;
		}
	}

	dbuf_writebyte(sctx->dcmpro->f, n);
	sctx->nbytes_written++;
}

static void lzss_init_window_lz5(struct de_lz77buffer *ringbuf)
{
	size_t wpos;
	int i;

	de_zeromem(ringbuf->buf, 4096);
	wpos = 13;
	for(i=1; i<256; i++) {
		de_memset(&ringbuf->buf[wpos], i, 13);
		wpos += 13;
	}
	for(i=0; i<256; i++) {
		ringbuf->buf[wpos++] = i;
	}
	for(i=255; i>=0; i--) {
		ringbuf->buf[wpos++] = i;
	}
	wpos += 128;
	de_memset(&ringbuf->buf[wpos], 0x20, 110);
	//wpos += 110;
}

// Used by lzss1 & hlp_lz77
static void lzss_fill_bitbuf(deark *c, struct lzss_ctx *sctx)
{
	u8 b;

	if(sctx->cur_ipos+1 > sctx->endpos) {
		sctx->stop_flag = 1;
		return;
	}
	b = dbuf_getbyte_p(sctx->dcmpri->f, &sctx->cur_ipos);
	de_bitbuf_lowlevel_add_byte(&sctx->bbll, b);
}

// Decompress Okumura LZSS and similar formats.
// codec_private_params = de_lzss1_params
// params->flags:
//   0x01: 0 = starting position=4096-18 [like Okumura LZSS]
//         1 = starting position=4096-16 [like MS SZDD]
//   0x02: 0 = init to all spaces
//         1 = LArc lz5 mode
void fmtutil_lzss1_codectype1(deark *c, struct de_dfilter_in_params *dcmpri,
	struct de_dfilter_out_params *dcmpro, struct de_dfilter_results *dres,
	void *codec_private_params)
{
#define LZSS_BUFSIZE 4096
	struct de_lzss1_params *params = (struct de_lzss1_params*)codec_private_params;
	struct lzss_ctx *sctx = NULL;

	sctx = de_malloc(c, sizeof(struct lzss_ctx));
	sctx->dcmpri = dcmpri;
	sctx->dcmpro = dcmpro;
	sctx->cur_ipos = dcmpri->pos;
	sctx->endpos = dcmpri->pos + dcmpri->len;
	sctx->ringbuf = de_lz77buffer_create(c, LZSS_BUFSIZE);
	sctx->ringbuf->writebyte_cb = lzss_lz77buf_writebytecb;
	sctx->ringbuf->userdata = (void*)sctx;

	if(params->flags & 0x2) {
		lzss_init_window_lz5(sctx->ringbuf);
	}
	else {
		de_lz77buffer_clear(sctx->ringbuf, 0x20);
	}
	sctx->ringbuf->curpos = (params->flags & 0x1) ? (LZSS_BUFSIZE-16) : (LZSS_BUFSIZE-18);

	sctx->bbll.is_lsb = 1;
	de_bitbuf_lowlevel_empty(&sctx->bbll);

	while(1) {
		u8 bit;

		if(sctx->bbll.nbits_in_bitbuf==0) {
			lzss_fill_bitbuf(c, sctx);
			if(sctx->stop_flag) goto unc_done;
		}

		bit = (u8)de_bitbuf_lowlevel_get_bits(&sctx->bbll, 1);
		if(bit) { // literal
			u8 b;

			if(sctx->cur_ipos+1 > sctx->endpos) goto unc_done;
			b = dbuf_getbyte_p(dcmpri->f, &sctx->cur_ipos);
			if(c->debug_level>=4) {
				de_dbg(c, "bpos=%u lit %u", sctx->ringbuf->curpos, (UI)b);
			}
			de_lz77buffer_add_literal_byte(sctx->ringbuf, b);
			if(sctx->stop_flag) goto unc_done;
		}
		else { // match
			UI x0, x1;
			UI matchpos;
			UI matchlen;

			if(sctx->cur_ipos+2 > sctx->endpos) goto unc_done;
			x0 = (UI)dbuf_getbyte_p(dcmpri->f, &sctx->cur_ipos);
			x1 = (UI)dbuf_getbyte_p(dcmpri->f, &sctx->cur_ipos);
			matchpos = ((x1 & 0xf0) << 4) | x0;
			matchlen = (x1 & 0x0f) + 3;
			if(c->debug_level>=4) {
				de_dbg(c, "bpos=%u match mpos=%u(%u) len=%u", sctx->ringbuf->curpos,
					matchpos, (UI)((LZSS_BUFSIZE-1)&(sctx->ringbuf->curpos-matchpos)), matchlen);
			}
			de_lz77buffer_copy_from_hist(sctx->ringbuf, matchpos, matchlen);
			if(sctx->stop_flag) goto unc_done;
		}
	}

unc_done:
	dres->bytes_consumed_valid = 1;
	dres->bytes_consumed = sctx->cur_ipos - dcmpri->pos;
	de_lz77buffer_destroy(c, sctx->ringbuf);
	de_free(c, sctx);
}

void fmtutil_decompress_lzss1(deark *c, struct de_dfilter_in_params *dcmpri,
	struct de_dfilter_out_params *dcmpro, struct de_dfilter_results *dres, UI flags)
{
	struct de_lzss1_params params;

	de_zeromem(&params, sizeof(struct de_lzss1_params));
	params.flags = flags;
	fmtutil_lzss1_codectype1(c, dcmpri, dcmpro, dres, &params);
}

//======================= hlp_lz77 =======================

// Very similar to fmtutil_lzss1_codectype1(). They could be consolidated,
// but it would be a bit messy.
void fmtutil_hlp_lz77_codectype1(deark *c, struct de_dfilter_in_params *dcmpri,
	struct de_dfilter_out_params *dcmpro, struct de_dfilter_results *dres,
	void *codec_private_params)
{
	struct lzss_ctx *sctx = NULL;

	sctx = de_malloc(c, sizeof(struct lzss_ctx));
	sctx->dcmpri = dcmpri;
	sctx->dcmpro = dcmpro;
	sctx->cur_ipos = dcmpri->pos;
	sctx->endpos = dcmpri->pos + dcmpri->len;
	sctx->ringbuf = de_lz77buffer_create(c, 4096);
	sctx->ringbuf->writebyte_cb = lzss_lz77buf_writebytecb;
	sctx->ringbuf->userdata = (void*)sctx;
	de_lz77buffer_clear(sctx->ringbuf, 0x20);

	sctx->bbll.is_lsb = 1;
	de_bitbuf_lowlevel_empty(&sctx->bbll);

	while(1) {
		u8 bit;

		if(sctx->bbll.nbits_in_bitbuf==0) {
			lzss_fill_bitbuf(c, sctx);
			if(sctx->stop_flag) goto unc_done;
		}

		bit = (u8)de_bitbuf_lowlevel_get_bits(&sctx->bbll, 1);
		if(bit==0) { // literal
			u8 b;

			if(sctx->cur_ipos+1 > sctx->endpos) goto unc_done;
			b = dbuf_getbyte_p(dcmpri->f, &sctx->cur_ipos);
			de_lz77buffer_add_literal_byte(sctx->ringbuf, b);
			if(sctx->stop_flag) goto unc_done;
		}
		else { // match
			UI x;
			UI matchpos;
			UI matchlen;

			if(sctx->cur_ipos+2 > sctx->endpos) goto unc_done;
			x = (UI)dbuf_getu16le_p(dcmpri->f, &sctx->cur_ipos);
			matchlen = (x>>12) + 3;
			matchpos = sctx->ringbuf->curpos - ((x & 0x0fff)+1);
			de_lz77buffer_copy_from_hist(sctx->ringbuf, matchpos, matchlen);
			if(sctx->stop_flag) goto unc_done;
		}
	}

unc_done:
	dres->bytes_consumed_valid = 1;
	dres->bytes_consumed = sctx->cur_ipos - dcmpri->pos;
	de_lz77buffer_destroy(c, sctx->ringbuf);
	de_free(c, sctx);
}

//========================================================

struct my_2layer_userdata {
	struct de_dfilter_ctx *dfctx_codec2;
	i64 intermediate_nbytes;
};

static void my_2layer_write_cb(dbuf *f, void *userdata,
	const u8 *buf, i64 size)
{
	struct my_2layer_userdata *u = (struct my_2layer_userdata*)userdata;

	de_dfilter_addbuf(u->dfctx_codec2, buf, size);
	u->intermediate_nbytes += size;
}

// If src indicates error and dst does not, copy the error from src to dst.
void de_dfilter_transfer_error(deark *c, struct de_dfilter_results *src,
	struct de_dfilter_results *dst)
{
	if(src->errcode && !dst->errcode) {
		dst->errcode = src->errcode;
		de_strlcpy(dst->errmsg, src->errmsg, sizeof(dst->errmsg));
	}
}

// Decompress an arbitrary two-layer compressed format.
// tlp->codec1* is the first one that will be used during decompression (i.e. the second
// method used when during *compression*).
void de_dfilter_decompress_two_layer(deark *c, struct de_dcmpr_two_layer_params *tlp)
{
	dbuf *outf_codec1 = NULL;
	struct de_dfilter_out_params dcmpro_codec1;
	struct de_dfilter_results dres_codec2;
	struct my_2layer_userdata u;
	struct de_dfilter_ctx *dfctx_codec2 = NULL;

	de_dfilter_init_objects(c, NULL, &dcmpro_codec1, NULL);
	de_dfilter_init_objects(c, NULL, NULL, &dres_codec2);
	de_zeromem(&u, sizeof(struct my_2layer_userdata));

	// Make a custom dbuf. The output from the first decompressor will be written
	// to it, and it will relay that output to the second decompressor.
	outf_codec1 = dbuf_create_custom_dbuf(c, 0, 0);
	dbuf_enable_wbuffer(outf_codec1);
	outf_codec1->userdata_for_customwrite = (void*)&u;
	outf_codec1->customwrite_fn = my_2layer_write_cb;

	dcmpro_codec1.f = outf_codec1;
	if(tlp->intermed_len_known) {
		dcmpro_codec1.len_known = 1;
		dcmpro_codec1.expected_len = tlp->intermed_expected_len;
	}
	else {
		dcmpro_codec1.len_known = 0;
		dcmpro_codec1.expected_len = 0;
	}

	dfctx_codec2 = de_dfilter_create(c, tlp->codec2, tlp->codec2_private_params, tlp->dcmpro, &dres_codec2);
	u.dfctx_codec2 = dfctx_codec2;

	// The first codec in the chain does not need the advanced (de_dfilter_create) API.
	if(tlp->codec1_type1) {
		tlp->codec1_type1(c, tlp->dcmpri, &dcmpro_codec1, tlp->dres, tlp->codec1_private_params);
	}
	else {
		de_dfilter_decompress_oneshot(c, tlp->codec1_pushable, tlp->codec1_private_params,
			tlp->dcmpri, &dcmpro_codec1, tlp->dres);
	}
	dbuf_flush(outf_codec1);
	de_dfilter_finish(dfctx_codec2);

	if(tlp->dres->errcode) goto done;
	de_dbg2(c, "size after intermediate decompression: %"I64_FMT, u.intermediate_nbytes);

	if(dres_codec2.errcode) {
		// An error occurred in codec2, and not in codec1.
		// Copy the error info to the dres that will be returned to the caller.
		de_dfilter_transfer_error(c, &dres_codec2, tlp->dres);
		goto done;
	}

done:
	de_dfilter_destroy(dfctx_codec2);
	dbuf_close(outf_codec1);
}

struct de_lz77buffer *de_lz77buffer_create(deark *c, UI bufsize)
{
	struct de_lz77buffer *rb;

	rb = de_malloc(c, sizeof(struct de_lz77buffer));
	rb->buf = de_malloc(c, (i64)bufsize);
	rb->bufsize = bufsize;
	rb->mask = bufsize - 1;
	return rb;
}

void de_lz77buffer_destroy(deark *c, struct de_lz77buffer *rb)
{
	if(!rb) return;
	de_free(c, rb->buf);
	de_free(c, rb);
}

// Set all bytes to the same value, and reset the current position to 0.
void de_lz77buffer_clear(struct de_lz77buffer *rb, UI val)
{
	de_memset(rb->buf, val, rb->bufsize);
	rb->curpos = 0;
}

void de_lz77buffer_set_curpos(struct de_lz77buffer *rb, UI newpos)
{
	rb->curpos = newpos & rb->mask;
}

void de_lz77buffer_add_literal_byte(struct de_lz77buffer *rb, u8 b)
{
	rb->writebyte_cb(rb, b);
	rb->buf[rb->curpos] = b;
	rb->curpos = (rb->curpos+1) & rb->mask;
}

void de_lz77buffer_copy_from_hist(struct de_lz77buffer *rb,
	UI startpos, UI count)
{
	UI frompos;
	UI i;

	frompos = startpos & rb->mask;
	for(i=0; i<count; i++) {
		de_lz77buffer_add_literal_byte(rb, rb->buf[frompos]);
		frompos = (frompos+1) & rb->mask;
	}
}

///////////////////////////////////
// "Squeeze"-style Huffman decoder

// The first node you add allows for 2 symbols, and each additional node adds 1.
// So in general, you need one less node than the number of symbols.
// The max number of symbols is 257: 256 byte values, plus a special "stop" code.
#define SQUEEZE_MAX_NODES 256

struct squeeze_data_item {
	i16 dval;
};

struct squeeze_node {
	u8 in_use;
	struct squeeze_data_item child[2];
};

struct squeeze_ctx {
	deark *c;
	struct de_dfilter_in_params *dcmpri;
	struct de_dfilter_out_params *dcmpro;
	struct de_dfilter_results *dres;
	const char *modname;
	i64 nbytes_written;
	i64 nodecount;
	struct fmtutil_huffman_decoder *ht;
	struct de_bitreader bitrd;
	struct squeeze_node tmpnodes[SQUEEZE_MAX_NODES]; // Temporary use when decoding the node table
};

static void squeeze_interpret_node(struct squeeze_ctx *sqctx,
	i64 nodenum, u64 currcode, UI currcode_nbits);

static void squeeze_interpret_dval(struct squeeze_ctx *sqctx,
	i16 dval, u64 currcode, UI currcode_nbits)
{
	char b2buf[72];

	if(dval>=0) { // a pointer to a node
		if((i64)dval < sqctx->nodecount) {
			squeeze_interpret_node(sqctx, (i64)dval, currcode, currcode_nbits);
		}
	}
	else if(dval>=(-257) && dval<=(-1)) {
		fmtutil_huffman_valtype adj_value;

		//  -257 => 256 (stop code)
		//  -256 => 255 (byte value)
		//  -255 => 254 (byte value)
		//  ...
		//  -1   => 0   (byte value)
		adj_value = -(((fmtutil_huffman_valtype)dval)+1);
		if(sqctx->c->debug_level>=3) {
			de_dbg3(sqctx->c, "code: \"%s\" = %d",
				de_print_base2_fixed(b2buf, sizeof(b2buf), currcode, currcode_nbits),
				(int)adj_value);
		}
		fmtutil_huffman_add_code(sqctx->c, sqctx->ht->bk, currcode, currcode_nbits, adj_value);
	}
	// TODO: Report errors?
}

static void squeeze_interpret_node(struct squeeze_ctx *sqctx,
	i64 nodenum, u64 currcode, UI currcode_nbits)
{
	// TODO: Report errors?
	if(nodenum<0 || nodenum>=sqctx->nodecount) return;
	if(sqctx->tmpnodes[nodenum].in_use) return; // Loops are bad
	if(currcode_nbits>=FMTUTIL_HUFFMAN_MAX_CODE_LENGTH) return;

	sqctx->tmpnodes[nodenum].in_use = 1;
	squeeze_interpret_dval(sqctx, sqctx->tmpnodes[nodenum].child[0].dval, currcode<<1, currcode_nbits+1);
	squeeze_interpret_dval(sqctx, sqctx->tmpnodes[nodenum].child[1].dval, ((currcode<<1) | 1), currcode_nbits+1);
	sqctx->tmpnodes[nodenum].in_use = 0;
}

static int squeeze_process_nodetable(deark *c, struct squeeze_ctx *sqctx)
{
	int retval = 0;

	// It feels a little wrong to go to the trouble of decoding this node table into
	// the form required by our Huffman library's API, when we know it's going to
	// just convert it back into a table much like it was originally. Maybe there
	// should be a better way to do this.
	de_dbg3(c, "interpreted huffman codebook:");
	de_dbg_indent(c, 1);
	squeeze_interpret_node(sqctx, 0, 0, 0);
	de_dbg_indent(c, -1);

	if(c->debug_level>=4) {
		fmtutil_huffman_dump(c, sqctx->ht);
	}

	retval = 1;
	return retval;
}

static int squeeze_read_nodetable(deark *c, struct squeeze_ctx *sqctx)
{
	i64 k;
	int retval = 0;

	if(sqctx->bitrd.curpos+2 > sqctx->bitrd.endpos) goto done;
	sqctx->nodecount = dbuf_getu16le_p(sqctx->dcmpri->f, &sqctx->bitrd.curpos);
	de_dbg(c, "node count: %d", (int)sqctx->nodecount);
	if(sqctx->nodecount > SQUEEZE_MAX_NODES) {
		de_dfilter_set_errorf(c, sqctx->dres, sqctx->modname,
			"Invalid node count");
		goto done;
	}

	de_dbg2(c, "node table nodes at %"I64_FMT, sqctx->bitrd.curpos);
	de_dbg_indent(c, 1);
	for(k=0; k<sqctx->nodecount; k++) {
		sqctx->tmpnodes[k].child[0].dval = (i16)dbuf_geti16le_p(sqctx->dcmpri->f, &sqctx->bitrd.curpos);
		sqctx->tmpnodes[k].child[1].dval = (i16)dbuf_geti16le_p(sqctx->dcmpri->f, &sqctx->bitrd.curpos);
		if(c->debug_level >= 2) {
			de_dbg2(c, "nodetable[%d]: %d %d", (int)k, (int)sqctx->tmpnodes[k].child[0].dval,
				(int)sqctx->tmpnodes[k].child[1].dval);
		}
	}
	de_dbg_indent(c, -1);
	if(sqctx->bitrd.curpos > sqctx->bitrd.endpos) goto done;

	if(!squeeze_process_nodetable(c, sqctx)) goto done;

	retval = 1;
done:
	return retval;
}

static int squeeze_read_codes(deark *c, struct squeeze_ctx *sqctx)
{
	int retval = 0;

	de_dbg(c, "huffman-compressed data at %"I64_FMT, sqctx->bitrd.curpos);
	sqctx->bitrd.bbll.is_lsb = 1;
	de_bitbuf_lowlevel_empty(&sqctx->bitrd.bbll);

	if(fmtutil_huffman_get_max_bits(sqctx->ht->bk) < 1) {
		// Empty tree? Assume this is an empty file.
		retval = 1;
		goto done;
	}

	while(1) {
		int ret;
		fmtutil_huffman_valtype val = 0;

		ret = fmtutil_huffman_read_next_value(sqctx->ht->bk, &sqctx->bitrd, &val, NULL);
		if(!ret || val<0 || val>256) {
			if(sqctx->bitrd.eof_flag) {
				retval = 1;
			}
			else {
				de_dfilter_set_errorf(c, sqctx->dres, sqctx->modname, "Huffman decode error");
			}
			goto done;
		}

		if(val>=0 && val<=255) {
			dbuf_writebyte(sqctx->dcmpro->f, (u8)val);
			sqctx->nbytes_written++;
			if(sqctx->dcmpro->len_known && (sqctx->nbytes_written >= sqctx->dcmpro->expected_len)) {
				retval = 1;
				goto done;
			}
		}
		else if(val==256) { // STOP code
			retval = 1;
			goto done;
		}
	}

done:
	return retval;
}

void fmtutil_huff_squeeze_codectype1(deark *c, struct de_dfilter_in_params *dcmpri,
	struct de_dfilter_out_params *dcmpro, struct de_dfilter_results *dres,
	void *codec_private_params)
{
	struct squeeze_ctx *sqctx = NULL;
	int ok = 0;

	sqctx = de_malloc(c, sizeof(struct squeeze_ctx));
	sqctx->c = c;
	sqctx->modname = "unsqueeze";
	sqctx->dcmpri = dcmpri;
	sqctx->dcmpro = dcmpro;
	sqctx->dres = dres;

	sqctx->bitrd.f = dcmpri->f;
	sqctx->bitrd.curpos = dcmpri->pos;
	sqctx->bitrd.endpos = dcmpri->pos + dcmpri->len;

	sqctx->ht = fmtutil_huffman_create_decoder(c, 257, 257);

	if(!squeeze_read_nodetable(c, sqctx)) goto done;
	if(!squeeze_read_codes(c, sqctx)) goto done;

	dres->bytes_consumed = sqctx->bitrd.curpos - dcmpri->pos;
	if(dres->bytes_consumed > dcmpri->len) {
		dres->bytes_consumed = dcmpri->len;
	}
	dres->bytes_consumed_valid = 1;
	ok = 1;

done:
	if(!ok || dres->errcode) {
		de_dfilter_set_errorf(c, dres, sqctx->modname, "Squeeze decompression failed");
	}

	if(sqctx) {
		fmtutil_huffman_destroy_decoder(c, sqctx->ht);
		de_free(c, sqctx);
	}
}

// Caller supplies a dbuf to write the decompressed table to.
// On failure, returns 0. Does not report an error.
int fmtutil_decompress_exepack_reloc_tbl(deark *c, i64 pos1, i64 endpos, dbuf *outf)
{
	i64 pos = pos1;
	i64 seg = 0;
	int reloc_count = 0;
	int retval = 0;
	int saved_indent_level;
	dbuf *inf = c->infile;

	de_dbg_indent_save(c, &saved_indent_level);
	de_dbg(c, "compressed reloc table: pos=%"I64_FMT", end=%"I64_FMT, pos1, endpos);
	if(endpos > inf->len) goto done;
	de_dbg_indent(c, 1);

	for(seg=0; seg<0x10000; seg+=0x1000) {
		i64 count;
		i64 i;

		if(pos>=endpos) {
			goto done;
		}
		count = dbuf_getu16le_p(inf, &pos);
		de_dbg2(c, "seg %04x count: %u", (UI)seg, (UI)count);

		de_dbg_indent(c, 1);
		for(i=0; i<count; i++) {
			i64 offs;

			if(pos>=endpos || reloc_count>=65535) goto done;
			offs = dbuf_getu16le_p(inf, &pos);
			de_dbg2(c, "reloc: %04x:%04x", (UI)seg, (UI)offs);
			dbuf_writeu16le(outf, offs);
			dbuf_writeu16le(outf, seg);
			reloc_count++;
		}
		de_dbg_indent(c, -1);
	}

	retval = 1;
	de_dbg(c, "reloc count: %d", (int)reloc_count);

done:
	de_dbg_indent_restore(c, saved_indent_level);
	return retval;
}
