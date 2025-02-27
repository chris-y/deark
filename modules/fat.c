// This file is part of Deark.
// Copyright (C) 2020 Jason Summers
// See the file COPYING for terms of use.

// FAT disk image
// LoadDskF/SaveDskF/SKF OS/2 disk image

#include <deark-private.h>
#include <deark-fmtutil.h>
DE_DECLARE_MODULE(de_module_fat);
DE_DECLARE_MODULE(de_module_loaddskf);

#include "../foreign/dskdcmps.h"

#define MAX_NESTING_LEVEL 16

struct member_data {
	u8 fn_base[8];
	u8 fn_ext[3];
	u8 is_subdir;
	u8 is_volume_label;
	u8 is_special;
	UI attribs;
	UI ea_handle;
	i64 fn_base_len, fn_ext_len;
	i64 filesize;
	i64 first_cluster;
	de_ucstring *short_fn;
	de_ucstring *long_fn;
	struct de_timestamp mod_time;
};

struct dirctx {
	u8 lfn_valid;
	u8 first_seq_num;
	u8 prev_seq_num;
	u8 name_cksum;
	i64 dir_entry_count;
	i64 pending_lfn_bytesused;
#define LFN_CHARS_PER_FRAGMENT 13
#define LFN_MAX_FRAGMENTS 20
	u8 pending_lfn[LFN_CHARS_PER_FRAGMENT*2*LFN_MAX_FRAGMENTS];
};

typedef struct localctx_struct {
	de_encoding input_encoding;
	u8 opt_check_root_dir;
	u8 prescan_root_dir;

	// TODO: Decide how to handle different variants of FAT.
#define FAT_SUBFMT_UNKNOWN   0
#define FAT_SUBFMT_PC        1
#define FAT_SUBFMT_ATARIST   2
	int subfmt_req;
	int subfmt;
#define FAT_PLATFORM_UNKNOWN   0
#define FAT_PLATFORM_PC        1
#define FAT_PLATFORM_ATARIST   2
	int platform;

	u8 num_fat_bits; // 12, 16, or 32. 0 if unknown.
	u8 has_atarist_checksum;
	i64 bytes_per_sector;
	i64 sectors_per_cluster;
	i64 bytes_per_cluster;
	i64 num_sectors;
	i64 data_region_sector;
	i64 data_region_pos;
	i64 num_data_region_clusters;
	i64 num_rsvd_sectors;
	i64 num_fats;
	i64 num_sectors_per_fat;
	i64 max_root_dir_entries16;
	i64 root_dir_sector;
	i64 num_cluster_identifiers;
	struct de_strarray *curpath;

	i64 num_fat_entries;
	u32 *fat_nextcluster; // array[num_fat_entries]
	u8 *cluster_used_flags; // array[num_fat_entries]
	u8 *cluster_used_flags_saved; // array[num_fat_entries] (or NULL)
	dbuf *ea_data; // NULL if not available
} lctx;

static void fat_save_cluster_use_flags(deark *c, lctx *d)
{
	if(!d->cluster_used_flags) return;
	if(!d->cluster_used_flags_saved) {
		d->cluster_used_flags_saved = de_malloc(c, d->num_fat_entries);
	}
	de_memcpy(d->cluster_used_flags_saved, d->cluster_used_flags,
		(size_t)d->num_fat_entries);
}

static void fat_restore_cluster_use_flags(deark *c, lctx *d)
{
	if(!d->cluster_used_flags_saved || !d->cluster_used_flags) return;
	de_memcpy(d->cluster_used_flags, d->cluster_used_flags_saved,
		(size_t)d->num_fat_entries);
}

static i64 sectornum_to_offset(deark *c, lctx *d, i64 secnum)
{
	return secnum * d->bytes_per_sector;
}

static int is_good_clusternum(lctx *d, i64 cnum)
{
	if(cnum<2) return 0;
	if(cnum >= d->num_cluster_identifiers) return 0;
	return 1;
}

static i64 clusternum_to_offset(deark *c, lctx *d, i64 cnum)
{
	return d->data_region_pos + (cnum-2) * d->bytes_per_cluster;
}

static void dbg_timestamp(deark *c, struct de_timestamp *ts, const char *name)
{
	char timestamp_buf[64];

	de_timestamp_to_string(ts, timestamp_buf, sizeof(timestamp_buf), 0);
	de_dbg(c, "%s: %s", name, timestamp_buf);
}

static i64 get_unpadded_len(const u8 *s, i64 len1)
{
	i64 i;
	i64 len = len1;

	// Stop at NUL, I guess.
	for(i=0; i<len1; i++) {
		if(s[i]==0x00) {
			len = i;
			break;
		}
	}

	for(i=len; i>0; i--) {
		if(s[i-1]!=' ') {
			return i;
		}
	}
	return 0;
}

static int extract_file_lowlevel(deark *c, lctx *d, struct member_data *md, dbuf *outf)
{
	int retval = 0;
	i64 cur_cluster;
	i64 nbytes_remaining;

	cur_cluster = md->first_cluster;
	if(md->is_subdir) {
		nbytes_remaining = 0;
	}
	else {
		nbytes_remaining = md->filesize;
	}

	while(1) {
		i64 dpos;
		i64 nbytes_to_copy;

		if(nbytes_remaining <= 0) break;
		if(!is_good_clusternum(d, cur_cluster)) break;
		if(d->cluster_used_flags[cur_cluster]) break;
		d->cluster_used_flags[cur_cluster] = 1;
		if(c->debug_level>=3) de_dbg3(c, "cluster: %d", (int)cur_cluster);
		dpos = clusternum_to_offset(c, d, cur_cluster);
		nbytes_to_copy = de_min_int(d->bytes_per_cluster, nbytes_remaining);
		dbuf_copy(c->infile, dpos, nbytes_to_copy, outf);
		nbytes_remaining -= nbytes_to_copy;
		cur_cluster = (i64)d->fat_nextcluster[cur_cluster];
	}

	if(nbytes_remaining>0) {
		goto done;
	}

	retval = 1;
done:
	return retval;
}

static void do_extract_file(deark *c, lctx *d, struct member_data *md)
{
	dbuf *outf = NULL;
	de_finfo *fi = NULL;
	de_ucstring *fullfn = NULL;

	if(!md->is_subdir) {
		if(md->filesize > d->num_data_region_clusters * d->bytes_per_cluster) {
			de_err(c, "%s: Bad file size", ucstring_getpsz_d(md->short_fn));
			goto done;
		}
	}

	fi = de_finfo_create(c);
	fullfn = ucstring_create(c);
	de_strarray_make_path(d->curpath, fullfn, DE_MPFLAG_NOTRAILINGSLASH);
	de_finfo_set_name_from_ucstring(c, fi, fullfn, DE_SNFLAG_FULLPATH);
	fi->original_filename_flag = 1;
	if(md->is_subdir) {
		fi->is_directory = 1;
	}
	else if(md->is_volume_label) {
		fi->is_volume_label = 1;
	}
	if(md->mod_time.is_valid) {
		fi->timestamp[DE_TIMESTAMPIDX_MODIFY] = md->mod_time;
	}

	outf = dbuf_create_output_file(c, NULL, fi, 0);

	if(!extract_file_lowlevel(c, d, md, outf)) {
		de_err(c, "%s: File extraction failed", ucstring_getpsz_d(md->short_fn));
		goto done;
	}

done:
	dbuf_close(outf);
	ucstring_destroy(fullfn);
	de_finfo_destroy(c, fi);
}

static void do_subdir(deark *c, lctx *d, struct member_data *md, int nesting_level);

static void do_vfat_entry(deark *c, lctx *d, struct dirctx *dctx, i64 pos1, u8 seq_num_raw)
{
	u8 seq_num;
	u8 fn_cksum;
	int is_first_entry = 0;
	i64 startpos_in_lfn;

	if(seq_num_raw==0xe5) {
		de_dbg(c, "[deleted VFAT entry]");
		dctx->lfn_valid = 0;
		goto done;
	}

	de_dbg(c, "seq number: 0x%02x", (UI)seq_num_raw);

	seq_num = seq_num_raw & 0xbf;

	if(seq_num<1 || seq_num>LFN_MAX_FRAGMENTS) {
		de_warn(c, "Bad VFAT sequence number (%u)", (UI)seq_num);
		dctx->lfn_valid = 0;
		goto done;
	}

	if(seq_num_raw & 0x40) {
		is_first_entry = 1;
		de_zeromem(dctx->pending_lfn, sizeof(dctx->pending_lfn));
		dctx->first_seq_num = seq_num;
		dctx->lfn_valid = 1;
	}
	else {
		if(!dctx->lfn_valid || (seq_num+1 != dctx->prev_seq_num)) {
			de_dbg(c, "[stray VFAT entry]");
			dctx->lfn_valid = 0;
			goto done;
		}
	}
	dctx->prev_seq_num = seq_num;

	startpos_in_lfn = LFN_CHARS_PER_FRAGMENT*2*((i64)seq_num-1);

	de_read(&dctx->pending_lfn[startpos_in_lfn+ 0], pos1+ 1, 10); // 5 chars
	fn_cksum = de_getbyte(pos1+13);
	de_read(&dctx->pending_lfn[startpos_in_lfn+10], pos1+14, 12); // 6 more chars
	de_read(&dctx->pending_lfn[startpos_in_lfn+22], pos1+28,  4); // 2 more chars
	de_dbg(c, "filename checksum (reported): 0x%02x", (UI)fn_cksum);
	if(!is_first_entry) {
		if(fn_cksum != dctx->name_cksum) {
			de_dbg(c, "[inconsistent VFAT checksums]");
			dctx->lfn_valid = 0;
		}
	}
	dctx->name_cksum = fn_cksum;

done:
	;
}

static void vfat_cksum_update(const u8 *buf, size_t buflen, u8 *cksum)
{
	size_t i;

	for(i=0; i<buflen; i++) {
		*cksum = (((*cksum) & 1) << 7) + ((*cksum) >> 1) + buf[i];
	}
}

// If the long file name seems valid, sets it in md->long_fn for later use.
static void handle_vfat_lfn(deark *c, lctx *d, struct dirctx *dctx,
	struct member_data *md)
{
	u8 cksum_calc = 0;
	i64 max_len_in_ucs2_chars;
	i64 len_in_ucs2_chars = 0;
	i64 i;

	if(!dctx->lfn_valid) goto done;
	if(dctx->prev_seq_num != 1) goto done;
	if(md->long_fn) goto done;

	vfat_cksum_update(md->fn_base, 8, &cksum_calc);
	vfat_cksum_update(md->fn_ext, 3, &cksum_calc);
	de_dbg(c, "filename checksum (calculated): 0x%02x", (UI)cksum_calc);
	if(cksum_calc != dctx->name_cksum) goto done;

	max_len_in_ucs2_chars = LFN_CHARS_PER_FRAGMENT * (i64)dctx->first_seq_num;
	if(max_len_in_ucs2_chars > (i64)(sizeof(dctx->pending_lfn)/2)) goto done;
	for(i=0; i<max_len_in_ucs2_chars; i++) {
		if(dctx->pending_lfn[i*2]==0x00 && dctx->pending_lfn[i*2+1]==0x00) break;
		if(dctx->pending_lfn[i*2]==0xff && dctx->pending_lfn[i*2+1]==0xff) break;
		len_in_ucs2_chars++;
	}

	md->long_fn = ucstring_create(c);
	ucstring_append_bytes(md->long_fn, dctx->pending_lfn, len_in_ucs2_chars*2,
		0, DE_ENCODING_UTF16LE);
	de_dbg(c, "long filename: \"%s\"", ucstring_getpsz_d(md->long_fn));

done:
	;
}

// Reads from md->fn_base* and md->fn_ext*, writes to md->short_fn
static void decode_short_filename(deark *c, lctx *d, struct member_data *md)
{
	if(md->fn_base_len>0) {
		ucstring_append_bytes(md->short_fn, md->fn_base, md->fn_base_len, 0, d->input_encoding);
	}
	else {
		ucstring_append_char(md->short_fn, '_');
	}
	if(md->fn_ext_len>0) {
		ucstring_append_char(md->short_fn, '.');
		ucstring_append_bytes(md->short_fn, md->fn_ext, md->fn_ext_len, 0, d->input_encoding);
	}
}

static void decode_volume_label_name(deark *c, lctx *d, struct member_data *md)
{
	if(md->fn_ext_len>0) {
		ucstring_append_bytes(md->short_fn, md->fn_base, 8, 0, d->input_encoding);
		ucstring_append_bytes(md->short_fn, md->fn_ext, md->fn_ext_len, 0, d->input_encoding);
	}
	else {
		ucstring_append_bytes(md->short_fn, md->fn_base, md->fn_base_len, 0, d->input_encoding);
	}
}

// md is that of the file whose EA data is being requested.
// Uses md->ea_handle.
static void do_fat_eadata_item(deark *c, lctx *d, struct member_data *md)
{
	de_module_params *mparams = NULL;

	if(!d->ea_data) goto done;
	if(md->ea_handle==0) goto done;
	mparams = de_malloc(c, sizeof(de_module_params));
	mparams->in_params.input_encoding = d->input_encoding;
	mparams->in_params.flags = 0x1;
	mparams->in_params.uint1 = (u32)md->ea_handle;
	de_dbg(c, "reading OS/2 extended attributes");
	de_dbg_indent(c, 1);
	// TODO: Better filenames for icons that may be extracted.
	de_run_module_by_id_on_slice(c, "ea_data", mparams, d->ea_data, 0, d->ea_data->len);
	de_dbg_indent(c, -1);

done:
	de_free(c, mparams);
}

// md is that of the "EA DATA" file itself.
static void do_fat_eadata(deark *c, lctx *d, struct member_data *md)
{
	int ret;

	if(d->ea_data) goto done;
	d->ea_data = dbuf_create_membuf(c, 0, 0);
	dbuf_set_length_limit(d->ea_data, c->infile->len);
	ret = extract_file_lowlevel(c, d, md, d->ea_data);
	if(!ret) {
		dbuf_close(d->ea_data);
		d->ea_data = NULL;
		goto done;
	}
	de_dbg(c, "[read EA data, len=%"I64_FMT"]", d->ea_data->len);
done:
	;
}

// Returns 0 if this is the end-of-directory marker.
static int do_dir_entry(deark *c, lctx *d, struct dirctx *dctx,
	i64 pos1, int nesting_level, int scanmode)
{
	u8 firstbyte;
	i64 ddate, dtime;
	int retval = 0;
	int is_deleted = 0;
	int need_curpath_pop = 0;
	de_ucstring *descr = NULL;
	struct member_data *md = NULL;

	md = de_malloc(c, sizeof(struct member_data));

	de_dbg(c, "dir entry at %"I64_FMT, pos1);
	de_dbg_indent(c, 1);

	de_read(md->fn_base, pos1+0, 8);
	de_read(md->fn_ext, pos1+8, 3);
	firstbyte = md->fn_base[0];

	if(firstbyte==0x00) {
		de_dbg(c, "[end of dir marker]");
		goto done;
	}
	retval = 1;

	md->attribs = (UI)de_getbyte(pos1+11);
	descr = ucstring_create(c);
	de_describe_dos_attribs(c, md->attribs, descr, 0x1);
	de_dbg(c, "attribs: 0x%02x (%s)", md->attribs, ucstring_getpsz_d(descr));
	if((md->attribs & 0x3f)==0x0f) {
		do_vfat_entry(c, d, dctx, pos1, firstbyte);
		goto done;
	}

	if((md->attribs & 0x18) == 0x00) {
		; // Normal file
	}
	else if((md->attribs & 0x18) == 0x08) {
		md->is_volume_label = 1;
	}
	else if((md->attribs & 0x18) == 0x10) {
		md->is_subdir = 1;
	}
	else {
		de_warn(c, "Invalid directory entry");
		md->is_special = 1;
		dctx->lfn_valid = 0;
		goto done;
	}

	if(dctx->lfn_valid) {
		handle_vfat_lfn(c, d, dctx, md);
		dctx->lfn_valid = 0;
	}

	if(firstbyte==0xe5) {
		de_dbg(c, "[deleted]");
		is_deleted = 1;
		md->fn_base[0] = '?';
	}
	else if(firstbyte==0x05) {
		md->fn_base[0] = 0xe5;
	}

	md->fn_base_len = get_unpadded_len(md->fn_base, 8);
	md->fn_ext_len = get_unpadded_len(md->fn_ext, 3);

	if(md->is_subdir && md->fn_base_len>=1 && md->fn_base[0]=='.') {
		// special "." and ".." dirs
		md->is_special = 1;
	}

	md->short_fn = ucstring_create(c);
	if(md->is_volume_label) {
		decode_volume_label_name(c, d, md);
	}
	else {
		decode_short_filename(c, d, md);
	}

	de_dbg(c, "filename: \"%s\"", ucstring_getpsz_d(md->short_fn));

	if(ucstring_isnonempty(md->long_fn)) {
		de_strarray_push(d->curpath, md->long_fn);
	}
	else {
		de_strarray_push(d->curpath, md->short_fn);
	}
	need_curpath_pop = 1;

	if(!scanmode && d->num_fat_bits<32) {
		md->ea_handle = (UI)de_getu16le(pos1+20);
		if(md->ea_handle) {
			de_dbg(c, "EA handle (if OS/2): %u", md->ea_handle);
		}
	}

	dtime = de_getu16le(pos1+22);
	ddate = de_getu16le(pos1+24);
	de_dos_datetime_to_timestamp(&md->mod_time, ddate, dtime);
	dbg_timestamp(c, &md->mod_time, "mod time");

	// TODO: This is wrong for FAT32.
	md->first_cluster = de_getu16le(pos1+26);
	de_dbg(c, "first cluster: %"I64_FMT, md->first_cluster);

	md->filesize = de_getu32le(pos1+28);
	de_dbg(c, "file size: %"I64_FMT, md->filesize);
	if(md->is_volume_label) md->filesize = 0;

	// (Done reading dir entry)

	if(is_deleted) goto done;

	if(scanmode) {
		if(!md->is_subdir && !md->is_special && (md->attribs&0x04) &&
			md->fn_base_len==7 && md->fn_ext_len==3 &&
			!de_memcmp(md->fn_base, "EA DATA", 7) &&
			!de_memcmp(md->fn_ext, " SF", 3) )
		{
			do_fat_eadata(c, d, md);
			goto done;
		}
		de_dbg2(c, "[scan mode - not extracting]");
		goto done;
	}

	if(md->ea_handle!=0 && d->ea_data) {
		do_fat_eadata_item(c, d, md);
	}

	if(!md->is_subdir && !md->is_special) {
		do_extract_file(c, d, md);
	}
	else if(md->is_subdir && !md->is_special) {
		do_extract_file(c, d, md);
		do_subdir(c, d, md, nesting_level+1);
	}

done:
	ucstring_destroy(descr);
	if(md) {
		ucstring_destroy(md->short_fn);
		ucstring_destroy(md->long_fn);
	}
	if(need_curpath_pop) {
		de_strarray_pop(d->curpath);
	}
	de_dbg_indent(c, -1);
	return retval;
}

// Process a contiguous block of directory entries
// Returns 0 if an end-of-dir marker was found.
static int do_dir_entries(deark *c, lctx *d, struct dirctx *dctx,
	i64 pos1, i64 len, int nesting_level, int scanmode)
{
	i64 num_entries;
	i64 i;
	int retval = 0;

	num_entries = len/32;
	de_dbg(c, "num entries: %"I64_FMT, num_entries);

	for(i=0; i<num_entries; i++) {
		if(!do_dir_entry(c, d, dctx, pos1+32*i, nesting_level, scanmode)) {
			goto done;
		}
		dctx->dir_entry_count++;
	}

	retval = 1;
done:
	return retval;
}

static void destroy_dirctx(deark *c, struct dirctx *dctx)
{
	if(!dctx) return;
	de_free(c, dctx);
}

static void do_subdir(deark *c, lctx *d, struct member_data *md, int nesting_level)
{
	int saved_indent_level;
	i64 cur_cluster_num;
	i64 cur_cluster_pos;
	struct dirctx *dctx = NULL;

	de_dbg_indent_save(c, &saved_indent_level);

	if(nesting_level >= MAX_NESTING_LEVEL) {
		de_err(c, "Directories nested too deeply");
		goto done;
	}

	dctx = de_malloc(c, sizeof(struct dirctx));

	cur_cluster_num = md->first_cluster;
	if(!is_good_clusternum(d, cur_cluster_num)) {
		de_err(c, "Bad subdirectory entry");
		goto done;
	}
	cur_cluster_pos = clusternum_to_offset(c, d, cur_cluster_num);
	de_dbg(c, "subdir starting at %"I64_FMT, cur_cluster_pos);
	de_dbg_indent(c, 1);

	while(1) {
		if(!is_good_clusternum(d, cur_cluster_num)) {
			break;
		}
		cur_cluster_pos = clusternum_to_offset(c, d, cur_cluster_num);
		de_dbg(c, "[subdir cluster %"I64_FMT" at %"I64_FMT"]", cur_cluster_num, cur_cluster_pos);

		if(d->cluster_used_flags[cur_cluster_num]) {
			goto done;
		}
		d->cluster_used_flags[cur_cluster_num] = 1;

		if(!do_dir_entries(c, d, dctx, cur_cluster_pos, d->bytes_per_cluster, nesting_level, 0)) {
			break;
		};

		cur_cluster_num = d->fat_nextcluster[cur_cluster_num];
	}

done:
	destroy_dirctx(c, dctx);
	de_dbg_indent_restore(c, saved_indent_level);
}

static void do_root_dir(deark *c, lctx *d)
{
	i64 pos1;
	struct dirctx *dctx = NULL;

	dctx = de_malloc(c, sizeof(struct dirctx));
	pos1 = sectornum_to_offset(c, d, d->root_dir_sector);
	de_dbg(c, "dir at %"I64_FMT, pos1);
	de_dbg_indent(c, 1);
	if(pos1<d->bytes_per_sector) goto done;
	if(d->prescan_root_dir) {
		de_dbg(c, "[scanning root dir]");
		// This feature causes us to intentionally read some clusters more than once,
		// so we have to work around our protections against doing that.
		fat_save_cluster_use_flags(c, d);
		de_dbg_indent(c, 1);
		(void)do_dir_entries(c, d, dctx, pos1, d->max_root_dir_entries16 * 32, 0, 1);
		de_dbg_indent(c, -1);
		fat_restore_cluster_use_flags(c, d);
		de_dbg(c, "[done scanning root dir]");
	}
	(void)do_dir_entries(c, d, dctx, pos1, d->max_root_dir_entries16 * 32, 0, 0);
done:
	destroy_dirctx(c, dctx);
	de_dbg_indent(c, -1);
}

static int root_dir_seems_valid(deark *c, lctx *d)
{
	i64 pos1;
	i64 max_entries_to_check;
	i64 i;
	i64 entrycount = 0;
	i64 errcount = 0;

	if(d->num_fat_bits==32) return 1;

	if(d->max_root_dir_entries16<=0) return 0;
	pos1 = sectornum_to_offset(c, d, d->root_dir_sector);
	if(pos1 + d->max_root_dir_entries16 * 32 > c->infile->len) {
		return 0;
	}

	max_entries_to_check = de_max_int(d->max_root_dir_entries16, 10);
	for(i=0; i<max_entries_to_check; i++) {
		i64 entrypos;
		u8 firstbyte;
		u8 attribs;

		entrypos = pos1 + 32*i;
		firstbyte = de_getbyte(entrypos);
		if(firstbyte==0x00) break;
		if(firstbyte==0xe5) continue; // Don't validate deleted entries
		entrycount++;
		attribs = de_getbyte(entrypos+11);
		if(attribs & 0xc0) {
			errcount++;
		}
		else if((attribs & 0x3f) == 0x0f) {
			; // LFN; OK
		}
		else if((attribs & 0x18)==0x18) {
			errcount++; // dir + vol.label not valid
		}

		// TODO: It's really lame to only validate the attribs field, when there's
		// so much more we could be doing. But it's a hard problem. We don't want
		// to be too sensitive to minor errors.
	}

	if(errcount>1 || (errcount==1 && entrycount<=1)) {
		return 0;
	}
	return 1;
}

static void do_atarist_boot_checksum(deark *c, lctx *d, i64 pos1)
{
	UI ck;

	ck = de_calccrc_oneshot(c->infile, pos1, 512, DE_CRCOBJ_SUM_U16BE);
	ck &= 0xffff;
	de_dbg(c, "Atari ST checksum: 0x%04x", ck);
	if(ck==0x1234) {
		d->has_atarist_checksum = 1;
	}
}

static void do_oem_name(deark *c, lctx *d, i64 pos, i64 len)
{
	struct de_stringreaderdata *srd;
	i64 i;

	srd = dbuf_read_string(c->infile, pos, len, len, 0, DE_ENCODING_ASCII);

	// Require printable ASCII.
	for(i=0; i<len; i++) {
		if(srd->sz[i]<32 || srd->sz[i]>126) {
			goto done;
		}
	}

	de_dbg(c, "OEM name: \"%s\"", ucstring_getpsz_d(srd->str));

done:
	de_destroy_stringreaderdata(c, srd);
}

static int do_boot_sector(deark *c, lctx *d, i64 pos1)
{
	i64 pos;
	i64 num_data_region_sectors;
	i64 num_root_dir_sectors;
	i64 num_sectors_per_fat16;
	i64 num_sectors_per_fat32 = 0;
	i64 num_sectors16;
	i64 num_sectors32 = 0;
	i64 num_sectors_per_track;
	i64 num_heads;
	i64 num_sectors_per_cylinder;
	i64 num_cylinders = 0;
	i64 jmpinstrlen;
	u8 b;
	u8 cksum_sig[2];
	int retval = 0;
	char tmpbuf[16];

	de_dbg(c, "boot sector at %"I64_FMT, pos1);
	de_dbg_indent(c, 1);

	// BIOS parameter block
	jmpinstrlen = (d->subfmt==FAT_SUBFMT_ATARIST)?2:3;
	de_dbg(c, "jump instr: %s",
		de_render_hexbytes_from_dbuf(c->infile, pos1, jmpinstrlen, tmpbuf, sizeof(tmpbuf)));

	if(d->subfmt==FAT_SUBFMT_ATARIST) {
		do_oem_name(c, d, pos1+2, 6);
		de_dbg(c, "serial num: %s",
			de_render_hexbytes_from_dbuf(c->infile, pos1+8, 3, tmpbuf, sizeof(tmpbuf)));
	}
	else {
		do_oem_name(c, d, pos1+3, 8);
	}

	pos = pos1+11;
	d->bytes_per_sector = de_getu16le_p(&pos);
	de_dbg(c, "bytes per sector: %d", (int)d->bytes_per_sector);
	d->sectors_per_cluster = (i64)de_getbyte_p(&pos);
	de_dbg(c, "sectors per cluster: %d", (int)d->sectors_per_cluster);
	d->num_rsvd_sectors = de_getu16le_p(&pos);

	de_dbg(c, "reserved sectors: %d", (int)d->num_rsvd_sectors);
	if(d->num_rsvd_sectors==0) {
		// This happens on some Atari ST disks. Don't know why.
		d->num_rsvd_sectors = 1;
	}

	d->num_fats = (i64)de_getbyte_p(&pos);
	de_dbg(c, "number of FATs: %d", (int)d->num_fats);

	// This is expected to be 0 for FAT32.
	d->max_root_dir_entries16 = de_getu16le_p(&pos);
	de_dbg(c, "max number of root dir entries (if FAT12/16): %d", (int)d->max_root_dir_entries16);

	num_sectors16 = de_getu16le_p(&pos);
	de_dbg(c, "number of sectors (old 16-bit field): %d", (int)num_sectors16);
	b = de_getbyte_p(&pos);
	de_dbg(c, "media descriptor: 0x%02x", (UI)b);
	num_sectors_per_fat16 = de_getu16le_p(&pos);
	de_dbg(c, "sectors per FAT (if FAT12/16): %d", (int)num_sectors_per_fat16);

	num_sectors_per_track = de_getu16le_p(&pos);
	de_dbg(c, "sectors per track: %d", (int)num_sectors_per_track);
	num_heads = de_getu16le_p(&pos);
	de_dbg(c, "number of heads: %d", (int)num_heads);

	pos = pos1+0x1fe;
	de_read(cksum_sig, pos, 2);
	de_dbg(c, "boot sector signature: %s",
		de_render_hexbytes_from_mem(cksum_sig, 2, tmpbuf, sizeof(tmpbuf)));

	do_atarist_boot_checksum(c, d, pos1);
	if(d->has_atarist_checksum) {
		d->platform = FAT_PLATFORM_ATARIST;
		de_dbg(c, "[This is probably a bootable Atari ST disk.]");
	}
	else if(cksum_sig[0]==0x55 && cksum_sig[1]==0xaa) {
		d->platform = FAT_PLATFORM_PC;
		de_dbg(c, "[Disk has PC-compatible boot code.]");
	}

	if(num_sectors16==0) {
		num_sectors32 = de_getu32le(pos1+32);
		de_dbg(c, "num sectors (new 32-bit field): %"I64_FMT, num_sectors32);
	}

	if(num_sectors_per_fat16==0) {
		num_sectors_per_fat32 = de_getu32le(pos1+36);
		de_dbg(c, "sectors per FAT (if FAT32): %u", (UI)num_sectors_per_fat32);
	}

	if(num_sectors_per_fat16==0) {
		d->num_sectors_per_fat = num_sectors_per_fat32;
	}
	else {
		d->num_sectors_per_fat = num_sectors_per_fat16;
	}

	if(num_sectors16==0) {
		d->num_sectors = num_sectors32;
	}
	else {
		d->num_sectors = num_sectors16;
	}

	num_sectors_per_cylinder = num_heads * num_sectors_per_track;
	if(num_sectors_per_cylinder > 0) {
		num_cylinders = de_pad_to_n(d->num_sectors, num_sectors_per_cylinder) / num_sectors_per_cylinder;
	}
	de_dbg(c, "number of cylinders (calculated): %"I64_FMT, num_cylinders);

	if(d->sectors_per_cluster<1) goto done;
	if(d->bytes_per_sector<32) goto done;
	d->bytes_per_cluster = d->bytes_per_sector * d->sectors_per_cluster;
	d->root_dir_sector = d->num_rsvd_sectors + d->num_sectors_per_fat * d->num_fats;
	de_dbg(c, "root dir pos (calculated): %"I64_FMT" (sector %"I64_FMT")",
		sectornum_to_offset(c, d, d->root_dir_sector), d->root_dir_sector);

	// num_root_dir_sectors is expected to be 0 for FAT32.
	num_root_dir_sectors = (d->max_root_dir_entries16*32 + d->bytes_per_sector - 1)/d->bytes_per_sector;

	num_data_region_sectors = d->num_sectors - (d->root_dir_sector + num_root_dir_sectors);
	if(num_data_region_sectors<0) goto done;
	d->num_data_region_clusters = num_data_region_sectors / d->sectors_per_cluster;
	de_dbg(c, "number of clusters (calculated): %"I64_FMT, d->num_data_region_clusters);

	d->data_region_sector = d->root_dir_sector + num_root_dir_sectors;
	d->data_region_pos = d->data_region_sector * d->bytes_per_sector;
	de_dbg(c, "data region pos (calculated): %"I64_FMT" (sector %"I64_FMT")", d->data_region_pos,
		d->data_region_sector);

	// (The first cluster is numbered "2".)
	d->num_cluster_identifiers = d->num_data_region_clusters + 2;

	if(d->num_data_region_clusters < 4085) {
		d->num_fat_bits = 12;
	}
	else if(d->num_data_region_clusters < 65525) {
		d->num_fat_bits = 16;
	}
	else {
		d->num_fat_bits = 32;
	}

	de_dbg(c, "bits per cluster id (calculated): %u", (UI)d->num_fat_bits);

	retval = 1;

done:
	if(!retval) {
		de_err(c, "Invalid or unsupported boot sector");
	}
	de_dbg_indent(c, -1);
	return retval;
}

static int do_read_fat(deark *c, lctx *d)
{
	i64 pos1;
	i64 pos;
	i64 fat_idx_to_read = 0;
	int retval = 0;
	i64 i;

	pos1 = sectornum_to_offset(c, d, d->num_rsvd_sectors + fat_idx_to_read*d->num_sectors_per_fat);
	de_dbg(c, "FAT#%d at %"I64_FMT, (int)fat_idx_to_read, pos1);
	de_dbg_indent(c, 1);

	if(d->num_cluster_identifiers > (i64)(DE_MAX_SANE_OBJECT_SIZE/sizeof(u32))) goto done;
	d->num_fat_entries = d->num_cluster_identifiers;
	d->fat_nextcluster = de_mallocarray(c, d->num_fat_entries, sizeof(u32));
	d->cluster_used_flags = de_malloc(c, d->num_fat_entries);

	pos = pos1;
	if(d->num_fat_bits==12) {
		for(i=0; i<d->num_fat_entries+1; i+=2) {
			UI val;

			val = (UI)dbuf_getint_ext(c->infile, pos, 3, 1, 0);
			pos += 3;
			if(i < d->num_fat_entries) {
				d->fat_nextcluster[i] = (u32)(val & 0xfff);
			}
			if(i+1 < d->num_fat_entries) {
				d->fat_nextcluster[i+1] = (u32)(val >> 12);
			}
		}
	}
	else if(d->num_fat_bits==16) {
		for(i=0; i<d->num_fat_entries; i++) {
			d->fat_nextcluster[i] = (u32)de_getu16le_p(&pos);
		}
	}
	else {
		de_err(c, "This type of FAT is not supported");
		goto done;
	}

	if(c->debug_level>=3) {
		for(i=0; i<d->num_fat_entries; i++) {
			de_dbg3(c, "fat[%"I64_FMT"]: %"I64_FMT, i, (i64)d->fat_nextcluster[i]);
		}
	}

	retval = 1;
done:
	de_dbg_indent(c, -1);
	return retval;
}

static void de_run_fat(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;
	const char *s;
	int got_root_dir = 0;
	de_encoding default_encoding =  DE_ENCODING_CP437_G;

	if(mparams) {
		// out_params.flags:
		//  0x1 = No valid FAT directory structure found
		mparams->out_params.flags = 0;
	}

	d = de_malloc(c, sizeof(lctx));

	d->prescan_root_dir = (u8)de_get_ext_option_bool(c, "fat:scanroot", 1);
	d->opt_check_root_dir = (u8)de_get_ext_option_bool(c, "fat:checkroot", 1);
	s = de_get_ext_option(c, "fat:subfmt");
	if(s) {
		if(!de_strcmp(s, "pc")) {
			d->subfmt_req = FAT_SUBFMT_PC;
		}
		else if(!de_strcmp(s, "atarist")) {
			d->subfmt_req = FAT_SUBFMT_ATARIST;
		}
	}
	d->subfmt = d->subfmt_req;
	if(d->subfmt==FAT_SUBFMT_ATARIST) {
		default_encoding = DE_ENCODING_ATARIST;
	}

	d->input_encoding = de_get_input_encoding(c, mparams, default_encoding);

	// TODO: Detect MBR?
	if(!do_boot_sector(c, d, 0)) goto done;
	if(d->num_fat_bits==0) goto done;

	switch(d->platform) {
	case FAT_PLATFORM_PC:
		de_declare_fmtf(c, "FAT%d - PC", d->num_fat_bits);
		break;
	case FAT_PLATFORM_ATARIST:
		de_declare_fmtf(c, "FAT%d - Atari ST", d->num_fat_bits);
		break;
	default:
		de_declare_fmtf(c, "FAT%d - Unknown platform", d->num_fat_bits);
		break;
	}

	if(!do_read_fat(c, d)) goto done;

	if(d->opt_check_root_dir) {
		if(!root_dir_seems_valid(c, d)) {
			de_warn(c, "This file does not appear to contain a valid FAT "
				"directory structure. (\"-opt fat:checkroot=0\" to try anyway)");
			goto done;
		}
	}

	d->curpath = de_strarray_create(c, MAX_NESTING_LEVEL+10);
	got_root_dir = 1;
	do_root_dir(c, d);

done:
	if(!got_root_dir) {
		// Inform the parent module that we failed to do anything.
		if(mparams) {
			mparams->out_params.flags |= 0x1;
		}
	}

	if(d) {
		de_free(c, d->fat_nextcluster);
		de_free(c, d->cluster_used_flags);
		de_free(c, d->cluster_used_flags_saved);
		if(d->curpath) de_strarray_destroy(d->curpath);
		if(d->ea_data) dbuf_close(d->ea_data);
		de_free(c, d);
	}
}

static int de_identify_fat(deark *c)
{
	i64 bytes_per_sector;
	i64 max_root_dir_entries;
	i64 num_rsvd_sectors;
	int confidence = 0;
	int has_pc_sig;
	int has_ext;
	u8 sectors_per_cluster;
	u8 num_fats;
	u8 media_descr;
	u8 b[32];

	// TODO: This needs a lot of work.
	// It's good enough for most FAT12 floppy disk images.

	de_read(b, 0, sizeof(b));
	bytes_per_sector = de_getu16le_direct(&b[11]);
	sectors_per_cluster = b[13];
	num_rsvd_sectors = de_getu16le_direct(&b[14]);
	num_fats = b[16];
	max_root_dir_entries = de_getu16le_direct(&b[17]);
	media_descr = b[21];

	if(bytes_per_sector!=512) return 0;
	switch(sectors_per_cluster) {
	case 1: case 2: case 4: case 8:
	case 16: case 32: case 64: case 128:
		break;
	default:
		return 0;
	}
	if(num_fats!=1 && num_fats!=2) return 0;
	if(media_descr<0xe5 && media_descr!=0) return 0; // Media descriptor

	confidence = 1;
	if(b[0]==0xeb && b[2]==0x90) confidence += 2;
	else if(b[0]==0xe9) confidence += 1;
	else if(b[0]==0x60) confidence += 1;
	has_pc_sig = (de_getu16be(510)==0x55aa);
	if(has_pc_sig) confidence += 2;
	if(num_fats==2) confidence += 1;
	if(media_descr>=0xe5) confidence += 1;
	if(num_rsvd_sectors==1) confidence += 1;
	if(max_root_dir_entries==112 || max_root_dir_entries==224) confidence += 2;

	has_ext = de_input_file_has_ext(c, "ima") ||
		de_input_file_has_ext(c, "img") ||
		de_input_file_has_ext(c, "st");

	if(confidence>=6) return (has_ext?100:80);
	else if(confidence>=4) return (has_ext?60:9);
	else return 0;
}

static void de_help_fat(deark *c)
{
	de_msg(c, "-opt fat:checkroot=0 : Read the directory structure, even if it "
		"seems invalid");
	de_msg(c, "-opt fat:scanroot=0 : Do not scan the root directory to look for "
		"special files");
}

void de_module_fat(deark *c, struct deark_module_info *mi)
{
	mi->id = "fat";
	mi->desc = "FAT disk image";
	mi->run_fn = de_run_fat;
	mi->identify_fn = de_identify_fat;
	mi->help_fn = de_help_fat;
}

///////////////////////// LoadDskF/SaveDskF format (OS/2-centric)

struct skf_ctx {
	int to_raw;
	int new_fmt;
	int is_compressed;
	u32 checksum_reported;
	i64 hdr_size;
	i64 expected_dcmpr_size; // 0 if unknown
	i64 padded_size; // 0 if unknown
};

static void loaddskf_pad_ima_file(deark *c, struct skf_ctx *d, dbuf *outf)
{
	i64 num_padding_bytes;
	u8 padding_value;

	dbuf_flush(outf);
	if(d->padded_size<=0) goto done;
	num_padding_bytes = d->padded_size - outf->len;
	if(num_padding_bytes<=0) goto done;
	de_dbg(c, "[adding padding]");

	// TODO: Does it matter what we pad with? Possibilities include 0x00, 0xe5, 0xf6.
	padding_value = 0x00;
	dbuf_write_run(outf, padding_value, num_padding_bytes);
done:
	;
}

static void loaddskf_convert_noncmpr_to_ima(deark *c, struct skf_ctx *d)
{
	dbuf *outf = NULL;

	outf = dbuf_create_output_file(c, "ima", NULL, 0);
	de_dbg(c, "[copying]");
	dbuf_copy(c->infile, d->hdr_size, c->infile->len - d->hdr_size, outf);
	loaddskf_pad_ima_file(c, d, outf);
	dbuf_close(outf);
}

static void loaddskf_decode_as_fat(deark *c, struct skf_ctx *d)
{
	i64 dlen;

	dlen = de_max_int(c->infile->len - d->hdr_size, d->padded_size);
	de_dbg(c, "decoding as FAT, pos=%"I64_FMT", len=%"I64_FMT, d->hdr_size, dlen);
	if(dlen<=0) goto done;

	de_dbg_indent(c, 1);
	de_run_module_by_id_on_slice(c, "fat", NULL, c->infile, d->hdr_size, dlen);
	de_dbg_indent(c, -1);
done:
	;
}

static void loaddskf_decompress(deark *c, struct skf_ctx *d)
{
	struct de_dfilter_in_params dcmpri;
	struct de_dfilter_out_params dcmpro;
	struct de_dfilter_results dres;
	dbuf *outf = NULL;

	if(d->to_raw) {
		outf = dbuf_create_output_file(c, "ima", NULL, 0);
		dbuf_enable_wbuffer(outf);
	}
	else {
		outf = dbuf_create_output_file(c, "unc.dsk", NULL, 0);
		dbuf_enable_wbuffer(outf);
		dbuf_write(outf, (const u8*)"\xaa\x59", 2);
		dbuf_copy(c->infile, 2, d->hdr_size-2, outf);
	}

	de_dfilter_init_objects(c, &dcmpri, &dcmpro, &dres);
	dcmpri.f = c->infile;
	dcmpri.pos = d->hdr_size;
	dcmpri.len = c->infile->len - dcmpri.pos;
	dcmpro.f = outf;

	de_dbg(c, "[decompressing]");
	dskdcmps_run(c, &dcmpri, &dcmpro, &dres);
	dbuf_flush(dcmpro.f);
	if(dres.errcode) {
		de_err(c, "Decompression failed: %s", de_dfilter_get_errmsg(c, &dres));
		goto done;
	}

	if(d->to_raw) {
		loaddskf_pad_ima_file(c, d, outf);
	}
	// TODO: If !d->to_raw, maybe we should still ensure it decompressed to
	// the expected size.

done:
	dbuf_close(outf);
}

static int loaddskf_read_header(deark *c, struct skf_ctx *d)
{
	i64 bytes_per_sector;
	i64 num_sectors_per_track;
	i64 num_cylinders;
	i64 num_heads;
	i64 num_sectors_in_image;
	int retval = 0;

	de_dbg(c, "header");
	de_dbg_indent(c, 1);

	bytes_per_sector = de_getu16le(4);
	de_dbg(c, "bytes per sector: %u", (UI)bytes_per_sector);
	d->checksum_reported = (u32)de_getu32le(20); // TODO: What is this?
	de_dbg(c, "checksum (reported): 0x%08x", (UI)d->checksum_reported);
	num_cylinders = de_getu16le(24);
	de_dbg(c, "cylinders: %u", (UI)num_cylinders);
	num_heads = de_getu16le(26);
	de_dbg(c, "number of heads: %u", (UI)num_heads);
	num_sectors_per_track = de_getu16le(28);
	de_dbg(c, "sectors per track: %u", (UI)num_sectors_per_track);
	num_sectors_in_image = de_getu16le(34);
	de_dbg(c, "num sectors in image: %u", (UI)num_sectors_in_image);

	d->hdr_size = de_getu16le(38);
	if(d->hdr_size==0) d->hdr_size = 512;
	de_dbg(c, "header size: %"I64_FMT, d->hdr_size);
	if(d->hdr_size<40 || d->hdr_size>c->infile->len) {
		goto done;
	}

	retval = 1;

	if(num_cylinders<20 || num_cylinders>200 ||
		num_heads<1 || num_heads>2 ||
		num_sectors_per_track<8 || num_sectors_per_track>200 ||
		bytes_per_sector<128 || bytes_per_sector>2048)
	{
		de_warn(c, "Unexpected disk geometry. Something may have failed.");
		goto done;
	}

	d->expected_dcmpr_size = num_sectors_in_image * bytes_per_sector;
	d->padded_size = num_cylinders * num_heads * num_sectors_per_track * bytes_per_sector;
	de_dbg(c, "expected uncmpr image size: %"I64_FMT", padded=%"I64_FMT,
		d->expected_dcmpr_size, d->padded_size);

	retval = 1;

done:
	if(!retval) {
		de_err(c, "Failed to parse LoadDskF file");
	}
	de_dbg_indent(c, -1);
	return retval;
}

static void de_run_loaddskf(deark *c, de_module_params *mparams)
{
	struct skf_ctx *d = NULL;
	const char *subfmt_name;
	UI sig;

	d = de_malloc(c, sizeof(struct skf_ctx));
	d->to_raw = de_get_ext_option_bool(c, "loaddskf:toraw", 0);

	sig = (UI)de_getu16be(0);
	switch(sig) {
	case 0xaa58:
		subfmt_name = "old";
		break;
	case 0xaa59:
		subfmt_name = "new";
		d->new_fmt = 1;
		break;
	case 0xaa5a:
		subfmt_name = "new, compressed";
		d->new_fmt = 1;
		d->is_compressed = 1;
		break;
	default:
		de_err(c, "Not a LoadDskF file");
		goto done;
	}

	de_declare_fmtf(c, "LoadDskF (%s)", subfmt_name);
	if(!loaddskf_read_header(c, d)) goto done;
	if(d->is_compressed) {
		loaddskf_decompress(c, d);
	}
	else {
		if(d->to_raw) {
			loaddskf_convert_noncmpr_to_ima(c, d);
		}
		else {
			loaddskf_decode_as_fat(c, d);
		}
	}

done:
	de_free(c, d);
}

static int de_identify_loaddskf(deark *c)
{
	UI sig;

	sig = (UI)de_getu16be(0);
	if(sig==0xaa58 || sig==0xaa59 || sig==0xaa5a) {
		if((UI)de_getu16be(2)==0xf000) {
			return 100;
		}
		return 9;
	}
	return 0;
}

static void de_help_loaddskf(deark *c)
{
	de_msg(c, "-opt loaddskf:toraw : Convert to raw FAT/IMA format");
}

void de_module_loaddskf(deark *c, struct deark_module_info *mi)
{
	mi->id = "loaddskf";
	mi->desc = "LoadDskF/SaveDskF disk image";
	mi->run_fn = de_run_loaddskf;
	mi->identify_fn = de_identify_loaddskf;
	mi->help_fn = de_help_loaddskf;
}
