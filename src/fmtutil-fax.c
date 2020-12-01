// This file is part of Deark.
// Copyright (C) 2020 Jason Summers
// See the file COPYING for terms of use.

// Fax3/Fax4/etc. decompressor

#define DE_NOT_IN_MODULE
#include "deark-config.h"
#include "deark-private.h"
#include "deark-fmtutil.h"

struct fax34_huffman_tree {
	struct fmtutil_huffman_tree *htwb[2]; // [0]=white, [1]=black
	struct fmtutil_huffman_tree *two_d_codes;
};

struct fax_ctx {
	struct de_dfilter_in_params *dcmpri;
	struct de_dfilter_out_params *dcmpro;
	struct de_dfilter_results *dres;
	const char *modname;
	struct de_fax34_params *fax34params;
	u8 has_eol_codes;
	u8 rows_padded_to_next_byte;
	u8 is_2d;

	i64 nbytes_written;
	struct de_bitreader bitrd;

	u8 *curr_row; // array[fax34params->image_width]
	u8 *prev_row; // array[fax34params->image_width]

	i64 rowspan_final;
	u8 *tmp_row_packed; // array[rowspan_final]

	i64 ypos; // Row of the next pixel to be decoded
	i64 a0; // Next output pixel x-position. Can be -1; the 2-d decoder sometimes
	// needs -1 and 0 to be distinct "reference pixel positions".

	u8 a0_color;
	i64 b1;
	i64 b2;
};

static void create_fax34_huffman_tree2d(deark *c, struct fax34_huffman_tree *f34ht)
{
	 f34ht->two_d_codes = fmtutil_huffman_create_tree(c, 16, 16);
#define FAX2D_EOFB       0
#define FAX2D_P          1
#define FAX2D_H          2
#define FAX2D_EXTENSION  3
#define FAX2D_V_BIAS     100
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x00, 7, FAX2D_EOFB); // EOFB...
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x01, 7, FAX2D_EXTENSION); // Extension...
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x02, 7, FAX2D_V_BIAS-3); // VL3
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x03, 7, FAX2D_V_BIAS+3); // VR3
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x02, 6, FAX2D_V_BIAS-2); // VL2
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x03, 6, FAX2D_V_BIAS+2); // VR2
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x1, 4, FAX2D_P); // P
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x1, 3, FAX2D_H); // H...
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x2, 3, FAX2D_V_BIAS-1); // VL1
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x3, 3, FAX2D_V_BIAS+1); // VR1
	 fmtutil_huffman_add_code(c, f34ht->two_d_codes, 0x1, 1, FAX2D_V_BIAS); // V0
}

static const i16 fax34whitevals[104] = {
	0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
	32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,
	60,61,62,63,64,128,192,256,320,384,448,512,576,640,704,768,832,896,960,1024,1088,1152,
	1216,1280,1344,1408,1472,1536,1600,1664,1728,1792,1856,1920,1984,2048,2112,2176,2240,
	2304,2368,2432,2496,2560
};
static const i16 fax34blackvals[104] = {
	0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
	32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,
	60,61,62,63,64,128,192,256,320,384,448,512,576,640,704,768,832,896,960,1024,1088,1152,
	1216,1280,1344,1408,1472,1536,1600,1664,1728,1792,1856,1920,1984,2048,2112,2176,2240,
	2304,2368,2432,2496,2560
};
static const u16 fax34whitecodes[104] = {
	0x35,0x7,0x7,0x8,0xb,0xc,0xe,0xf,0x13,0x14,0x7,0x8,0x8,0x3,0x34,0x35,0x2a,0x2b,0x27,
	0xc,0x8,0x17,0x3,0x4,0x28,0x2b,0x13,0x24,0x18,0x2,0x3,0x1a,0x1b,0x12,0x13,0x14,0x15,
	0x16,0x17,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x4,0x5,0xa,0xb,0x52,0x53,0x54,0x55,0x24,0x25,
	0x58,0x59,0x5a,0x5b,0x4a,0x4b,0x32,0x33,0x34,0x1b,0x12,0x17,0x37,0x36,0x37,0x64,0x65,
	0x68,0x67,0xcc,0xcd,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xdb,0x98,0x99,0x9a,
	0x18,0x9b,0x8,0xc,0xd,0x12,0x13,0x14,0x15,0x16,0x17,0x1c,0x1d,0x1e,0x1f
};
static const u16 fax34blackcodes[104] = {
	0x37,0x2,0x3,0x2,0x3,0x3,0x2,0x3,0x5,0x4,0x4,0x5,0x7,0x4,0x7,0x18,0x17,0x18,0x8,0x67,
	0x68,0x6c,0x37,0x28,0x17,0x18,0xca,0xcb,0xcc,0xcd,0x68,0x69,0x6a,0x6b,0xd2,0xd3,0xd4,
	0xd5,0xd6,0xd7,0x6c,0x6d,0xda,0xdb,0x54,0x55,0x56,0x57,0x64,0x65,0x52,0x53,0x24,0x37,
	0x38,0x27,0x28,0x58,0x59,0x2b,0x2c,0x5a,0x66,0x67,0xf,0xc8,0xc9,0x5b,0x33,0x34,0x35,
	0x6c,0x6d,0x4a,0x4b,0x4c,0x4d,0x72,0x73,0x74,0x75,0x76,0x77,0x52,0x53,0x54,0x55,0x5a,
	0x5b,0x64,0x65,0x8,0xc,0xd,0x12,0x13,0x14,0x15,0x16,0x17,0x1c,0x1d,0x1e,0x1f
};
static const u8 fax34whitecodelengths[104] = {
	8,6,4,4,4,4,4,4,5,5,5,5,6,6,6,6,6,6,7,7,7,7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
	8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,5,5,6,7,8,8,8,8,8,8,9,9,9,9,9,9,9,9,9,9,9,9,
	9,9,9,6,9,11,11,11,12,12,12,12,12,12,12,12,12,12
};
static const u8 fax34blackcodelengths[104] = {
	10,3,2,2,3,4,4,5,6,6,7,7,7,8,8,9,10,10,10,11,11,11,11,11,11,11,12,12,12,12,12,12,12,
	12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,
	12,12,12,10,12,12,12,12,12,12,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,
	13,13,11,11,11,12,12,12,12,12,12,12,12,12,12
};

static struct fax34_huffman_tree *create_fax34_huffman_tree(deark *c, int need_2d)
{
	 struct fax34_huffman_tree *f34ht;
	 i64 i;
	 static const i64 num_white_codes = (i64)DE_ARRAYCOUNT(fax34whitecodes);
	 static const i64 num_black_codes = (i64)DE_ARRAYCOUNT(fax34blackcodes);

	 f34ht = de_malloc(c, sizeof(struct fax34_huffman_tree));
	 f34ht->htwb[0] = fmtutil_huffman_create_tree(c, num_white_codes+10, 0);
	 f34ht->htwb[1] = fmtutil_huffman_create_tree(c, num_black_codes+10, 0);

	 if(need_2d) {
		 create_fax34_huffman_tree2d(c, f34ht);
	 }

	 for(i=0; i<num_white_codes; i++) {
		 fmtutil_huffman_add_code(c, f34ht->htwb[0], (u64)fax34whitecodes[i],
			 (UI)fax34whitecodelengths[i], (fmtutil_huffman_valtype)fax34whitevals[i]);
	 }
	 for(i=0; i<num_black_codes; i++) {
		 fmtutil_huffman_add_code(c, f34ht->htwb[1], (u64)fax34blackcodes[i],
			 (UI)fax34blackcodelengths[i], (fmtutil_huffman_valtype)fax34blackvals[i]);
	 }

	 // 8 or more 0s = EOL or sync bits
	 fmtutil_huffman_add_code(c, f34ht->htwb[0], 0, 8, -1);
	 fmtutil_huffman_add_code(c, f34ht->htwb[1], 0, 8, -1);

	 return f34ht;
}

static void destroy_fax34_huffman_tree(deark *c, struct fax34_huffman_tree *f34ht)
{
	if(!f34ht) return;
	fmtutil_huffman_destroy_tree(c, f34ht->htwb[0]);
	fmtutil_huffman_destroy_tree(c, f34ht->htwb[1]);
	if(f34ht->two_d_codes) fmtutil_huffman_destroy_tree(c, f34ht->two_d_codes);
	de_free(c, f34ht);
}

static void fax34_on_eol(deark *c, struct fax_ctx *fc)
{
	i64 i;

	de_dbg3(c, "EOL");

	if(fc->ypos >= fc->fax34params->image_height) {
		fc->a0 = -1;
		fc->a0_color = 0;
		return;
	}

	// [Pack curr_row into bits (tmp_row_packed)]
	de_zeromem(fc->tmp_row_packed, (size_t)fc->rowspan_final);
	for(i=0; i<fc->fax34params->image_width; i++) {
		if(fc->curr_row[i]) {
			fc->tmp_row_packed[i/8] |= 1U<<(7-i%8);
		}
	}

	// [Write tmp_row_packed to fc->dcmpro]
	dbuf_write(fc->dcmpro->f, fc->tmp_row_packed, fc->rowspan_final);
	fc->nbytes_written += fc->rowspan_final;

	if(fc->is_2d) {
		de_memcpy(fc->prev_row, fc->curr_row, (size_t)fc->fax34params->image_width);
	}

	// initialize curr_row
	de_zeromem(fc->curr_row, (size_t)fc->fax34params->image_width);

	fc->ypos++;
	fc->a0 = -1;
	fc->a0_color = 0;
}

// Record run_len pixels, updating fc->a0.
// If fc->a0 == -1, sets it to 0 first.
static void fax34_record_run(deark *c, struct fax_ctx *fc, u8 color, i64 run_len)
{
	i64 i;

	de_dbg3(c, "run c=%u len=%d", (UI)color, (int)run_len);
	if(fc->a0<0) {
		fc->a0 = 0;
	}
	for(i=0; (i<run_len) && (fc->a0 < fc->fax34params->image_width); i++) {
		fc->curr_row[fc->a0++] = color;
	}
}

// Record run_len pixels, updating fc->a0.
// If fc->a0 == -1, only sets rec_len-1 pixels.
static void fax34_record_run_using_a0(deark *c, struct fax_ctx *fc, i64 run_len)
{
	if(fc->a0<0) {
		run_len += fc->a0;
		fc->a0 = 0;
	}
	fax34_record_run(c, fc, fc->a0_color, run_len);
}

static void init_fax34_bitreader(deark *c, struct fax_ctx *fc)
{
	de_zeromem(&fc->bitrd, sizeof(struct de_bitreader));
	fc->bitrd.f = fc->dcmpri->f;
	fc->bitrd.curpos = fc->dcmpri->pos;
	fc->bitrd.endpos = fc->dcmpri->pos + fc->dcmpri->len;
	fc->bitrd.bbll.is_lsb = fc->fax34params->is_lsb;
}

// Read up to and including the next '1' bit
static int fax34_finish_sync(deark *c, struct fax_ctx *fc, i64 max_bits_to_search)
{
	i64 nbits_searched = 0;
	int retval = 0;

	while(1) {
		u64 n;

		if(nbits_searched >= max_bits_to_search) {
			goto done;
		}

		n = de_bitreader_getbits(&fc->bitrd, 1);
		if(fc->bitrd.eof_flag) goto done;
		nbits_searched++;
		if(n!=0) {
			retval = 1;
			goto done;
		}
	}
done:
	return retval;
}

// Read up to and including run of 8 or more '0' bits, followed by a '1' bit.
static int fax34_full_sync(deark *c, struct fax_ctx *fc, i64 max_bits_to_search)
{
	i64 nbits_searched = 0;
	UI zcount = 0;
	int retval = 0;

	while(1) {
		u64 n;

		if(nbits_searched >= max_bits_to_search) {
			goto done;
		}
		n = de_bitreader_getbits(&fc->bitrd, 1);
		if(fc->bitrd.eof_flag) goto done;
		nbits_searched++;

		if(n!=0) {
			zcount = 0;
			continue;
		}
		zcount++;
		if(zcount>=8) {
			break;
		}
	}

	retval = fax34_finish_sync(c, fc, max_bits_to_search-nbits_searched);

done:
	return retval;
}

// Sets fc->b1 appropriately, according to fc->a0 and fc->prev_row and
// fc->fax34params->image_width.
static void find_b1(struct fax_ctx *fc)
{
	i64 i;

	for(i=fc->a0+1; i<fc->fax34params->image_width; i++) {
		if(i<0) continue;

		// first prev_row pixel to the right of a0, of opposite color to a0_color
		if(fc->prev_row[i]==fc->a0_color) continue;

		// Looking for a "changing element". I.e. if both it and the pixel to the left
		// exist, they must have different colors.
		if(i==0) {
			// Leftmost pixel is "changing" if it is black
			if(fc->prev_row[i]==0) continue;
		}
		else {
			if(fc->prev_row[i-1] == fc->prev_row[i]) continue;
		}

		fc->b1 = i;
		return;
	}
	fc->b1 = fc->fax34params->image_width;
}

static void find_b1_and_b2(struct fax_ctx *fc)
{
	i64 i;

	find_b1(fc);

	for(i=fc->b1+1; i<fc->fax34params->image_width; i++) {
		// Looking for a "changing element", i.e. one having a different color from
		// the pixel to its left.
		if(fc->prev_row[i-1] == fc->prev_row[i]) continue;
		fc->b2 = i;
		return;
	}
	fc->b2 = fc->fax34params->image_width;
}

static void do_decompress_fax34(deark *c, struct fax_ctx *fc,
	struct fax34_huffman_tree *f34ht)
{
	i64 pending_run_len = 0;
	UI f2d_h_codes_remaining = 0;
	char errmsg[100];

	errmsg[0] = '\0';
	init_fax34_bitreader(c, fc);

	if(fc->has_eol_codes) {
		if(!fax34_full_sync(c, fc, 1024)) {
			if(fc->fax34params->tiff_cmpr_meth==3) {
				de_dbg(c, "[no sync mark found, trying to compensate]");
				fc->has_eol_codes = 0;
				fc->rows_padded_to_next_byte = 0;
				init_fax34_bitreader(c, fc);
			}
			else {
				de_snprintf(errmsg, sizeof(errmsg), "Failed to find sync mark");
				goto done;
			}
		}
	}

	fc->ypos = 0;
	fc->a0 = -1;
	fc->a0_color = 0;

	while(1) {
		int ret;
		fmtutil_huffman_valtype val = 0;

		if(fc->ypos >= fc->fax34params->image_height ||
			((fc->ypos == fc->fax34params->image_height-1) && (fc->a0 >= fc->fax34params->image_width)))
		{
			goto done; // Normal completion
		}
		if(fc->dcmpro->len_known && fc->nbytes_written>fc->dcmpro->expected_len) {
			goto done; // Sufficient output
		}

		if(fc->bitrd.eof_flag) {
			de_snprintf(errmsg, sizeof(errmsg), "Unexpected end of compressed data");
			goto done;
		}

		if(!fc->has_eol_codes && (fc->a0 >= fc->fax34params->image_width)) {
			if(fc->rows_padded_to_next_byte) {
				de_bitbuf_lowelevel_empty(&fc->bitrd.bbll);
			}
			fax34_on_eol(c, fc);
			pending_run_len = 0;
			f2d_h_codes_remaining = 0;
		}

		if(fc->is_2d && f2d_h_codes_remaining==0) {
			ret = fmtutil_huffman_read_next_value(f34ht->two_d_codes, &fc->bitrd, &val, NULL);
			if(!ret) {
				if(fc->bitrd.eof_flag) {
					de_snprintf(errmsg, sizeof(errmsg), "Unexpected end of compressed data");
				}
				else {
					de_snprintf(errmsg, sizeof(errmsg), "Huffman decode error");
				}
				goto done;
			}
			de_dbg3(c, "val: %d", (int)val);

			if(val>=FAX2D_V_BIAS-3 && val<=FAX2D_V_BIAS+3) { // VL(3), ..., V(0), ..., VR(3)
				i64 run_len;

				find_b1(fc);
				de_dbg3(c, "at %d b1=%d", (int)fc->a0, (int)fc->b1);
				run_len = fc->b1 - fc->a0 + ((i64)val-FAX2D_V_BIAS);

				fax34_record_run_using_a0(c, fc, run_len);
				fc->a0_color = fc->a0_color?0:1;
			}
			else if(val==FAX2D_P) {
				find_b1_and_b2(fc);
				fax34_record_run_using_a0(c, fc, fc->b2 - fc->a0);
			}
			else if(val==FAX2D_H) {
				f2d_h_codes_remaining = 2;
			}
			else if(val==FAX2D_EOFB) {
				de_dbg3(c, "EOFB");
				goto done;
			}
			else {
				de_dbg3(c, "stopping (%d)", (int)val);
				goto done;
			}
		}
		else {
			ret = fmtutil_huffman_read_next_value(f34ht->htwb[(UI)fc->a0_color], &fc->bitrd, &val, NULL);
			if(!ret) {
				if(fc->bitrd.eof_flag) {
					de_snprintf(errmsg, sizeof(errmsg), "Unexpected end of compressed data");
				}
				else {
					de_snprintf(errmsg, sizeof(errmsg), "Huffman decode error");
				}
				goto done;
			}

			if(val<0) {
				if(!fc->has_eol_codes) {
					de_snprintf(errmsg, sizeof(errmsg), "Huffman decode error");
					goto done;
				}

				if(!fax34_finish_sync(c, fc, 64)) {
					de_snprintf(errmsg, sizeof(errmsg), "Failed to find EOL mark");
					goto done;
				}
				fax34_on_eol(c, fc);
				pending_run_len = 0;
			}
			else if(val<64) {
				pending_run_len += (i64)val;
				fax34_record_run(c, fc, fc->a0_color, pending_run_len);
				pending_run_len = 0;
				fc->a0_color = fc->a0_color?0:1;
				if(f2d_h_codes_remaining>0) {
					f2d_h_codes_remaining--;
				}
			}
			else { // make-up code
				pending_run_len += (i64)val;
			}
		}
	}

done:
	fax34_on_eol(c, fc); // Make sure we emit the last row

	if(errmsg[0]) {
		if(fc->ypos>0) {
			de_warn(c, "[%s] Failed to decode entire strip: %s", fc->modname, errmsg);
		}
		else {
			de_dfilter_set_errorf(c, fc->dres, fc->modname, "%s", errmsg);
		}
	}
}

void fmtutil_fax34_codectype1(deark *c, struct de_dfilter_in_params *dcmpri,
	struct de_dfilter_out_params *dcmpro, struct de_dfilter_results *dres,
	void *codec_private_params)
{
	struct fax_ctx *fc = NULL;
	struct fax34_huffman_tree *f34ht = NULL;

	fc = de_malloc(c, sizeof(struct fax_ctx));
	fc->modname = "fax_decode";
	fc->fax34params = (struct de_fax34_params*)codec_private_params;
	fc->dcmpri = dcmpri;
	fc->dcmpro = dcmpro;
	fc->dres = dres;

	if((fc->fax34params->tiff_cmpr_meth==3 && (fc->fax34params->t4options & 0x1)) ||
		(fc->fax34params->tiff_cmpr_meth==4))
	{
		fc->is_2d = 1;
	}

	if(fc->fax34params->tiff_cmpr_meth==2) {
		fc->has_eol_codes = 0;
		fc->rows_padded_to_next_byte = 1;
	}
	else if(fc->fax34params->tiff_cmpr_meth==3) {
		fc->has_eol_codes = 1;
	}

	if(fc->is_2d && fc->fax34params->tiff_cmpr_meth==4) {
		;
	}
	else if(fc->is_2d) {
		de_dfilter_set_errorf(c, fc->dres, fc->modname, "This type of fax compression "
			"is not supported");
		goto done;
	}

	if(fc->fax34params->image_width < 1 ||
		fc->fax34params->image_width > c->max_image_dimension)
	{
		goto done;
	}
	fc->rowspan_final = (fc->fax34params->image_width+7)/8;

	fc->curr_row = de_malloc(c, fc->fax34params->image_width);
	fc->prev_row = de_malloc(c, fc->fax34params->image_width);
	fc->tmp_row_packed = de_malloc(c, fc->rowspan_final);

	f34ht = create_fax34_huffman_tree(c, (int)fc->is_2d);
	do_decompress_fax34(c, fc, f34ht);

done:
	destroy_fax34_huffman_tree(c, f34ht);
	if(fc) {
		de_free(c, fc->curr_row);
		de_free(c, fc->prev_row);
		de_free(c, fc->tmp_row_packed);
		de_free(c, fc);
	}
}
