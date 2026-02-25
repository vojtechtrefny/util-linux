/*
 * DASD (IBM mainframe disk) partition table probing
 *
 * Supports CDL (Compatible Disk Layout) with VTOC and
 * LDL (Linux Disk Layout) with a single implicit partition.
 *
 * Copyright (C) 2025 Red Hat, Inc.
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>

#include "pt-dasd.h"
#include "partitions.h"

/*
 * Convert VOLSER from EBCDIC to ASCII.
 */
static void dasd_get_volser(const struct dasd_volume_label *vlabel, char *volser)
{
	int i;

	for (i = 0; i < DASD_VOLSER_LENGTH; i++)
		volser[i] = dasd_ebcdic_to_ascii[(unsigned char) vlabel->volid[i]];
	volser[DASD_VOLSER_LENGTH] = '\0';

	/* trim trailing spaces */
	for (i = DASD_VOLSER_LENGTH - 1; i >= 0 && volser[i] == ' '; i--)
		volser[i] = '\0';
}

/*
 * Convert DS1DSNAM (data set name) from EBCDIC to ASCII.
 * @dsnam must be at least sizeof(f1->DS1DSNAM) + 1 bytes.
 */
static void dasd_get_dsnam(const struct dasd_format1_label *f1, char *dsnam)
{
	for (int i = 0; i < (int) sizeof(f1->DS1DSNAM); i++)
		dsnam[i] = dasd_ebcdic_to_ascii[(unsigned char) f1->DS1DSNAM[i]];
	dsnam[sizeof(f1->DS1DSNAM)] = '\0';

	/* trim trailing spaces */
	for (int i = sizeof(f1->DS1DSNAM) - 1; i >= 0 && dsnam[i] == ' '; i--)
		dsnam[i] = '\0';
}

/*
 * Check whether a block contains a Format 4 DSCB (VTOC header).
 *
 * Format 4 is identified by:
 *   - 44-byte key field filled with 0x04
 *   - DS4IDFMT == 0xf4
 */
static int is_dasd_f4_label(const unsigned char *buf)
{
	int i;

	for (i = 0; i < DASD_F4_KEYCD_LENGTH; i++) {
		if (buf[i] != DASD_F4_KEYCD_BYTE)
			return 0;
	}
	if (buf[DASD_F4_KEYCD_LENGTH] != DASD_FMT_ID_F4)
		return 0;

	return 1;
}

/*
 * Parse CDL (VOL1) partition table.
 *
 * Scans for the Format 4 label to determine geometry, then reads
 * Format 1/8 labels to enumerate partitions (max 3).
 */
static int probe_dasd_pt_cdl(blkid_probe pr, blkid_partlist ls,
			     blkid_parttable tab,
			     unsigned int blocksize)
{
	const struct dasd_format4_label *f4;
	const struct dasd_format1_label *f1;
	const unsigned char *buf;
	unsigned int f4_blk = 0;
	unsigned int blk_per_trk;
	uint16_t heads;
	uint32_t cylinders;
	unsigned int blk;
	int partno = 0;

	/*
	 * Scan blocks 3..20 looking for the Format 4 label.
	 * On a real DASD, it is at CC=0 HH=1 R=1, which maps to
	 * linux block = blk_per_trk (one track after the start).
	 * Since we don't know blk_per_trk yet, just scan.
	 */
	for (blk = 3; blk <= 20; blk++) {
		buf = blkid_probe_get_buffer(pr,
				(uint64_t) blk * blocksize,
				sizeof(struct dasd_format4_label));
		if (!buf) {
			if (errno)
				return -errno;
			return BLKID_PROBE_NONE;
		}
		if (is_dasd_f4_label(buf)) {
			f4_blk = blk;
			break;
		}
	}

	if (!f4_blk) {
		DBG(LOWPROBE, ul_debug("DASD: CDL detected but no F4 label found"));
		return BLKID_PROBE_NONE;
	}

	f4 = (const struct dasd_format4_label *) buf;

	/*
	 * Derive blocks-per-track from the F4 label position.
	 * F4 is at CC=0, HH=1, R=1. The linux block number equals
	 * blk_per_trk * 1 (track 1) + 0 (block offset, R=1 is first data).
	 * Actually, F4 is at linux block = blk_per_trk exactly.
	 */
	blk_per_trk = f4_blk;

	heads = be16_to_cpu(f4->DS4DEVCT.DS4DSTRK);
	cylinders = be16_to_cpu(f4->DS4DEVCT.DS4DSCYL);

	/* For large volumes, use DS4DCYL if compatible cylinder count */
	if (cylinders == DASD_LV_COMPAT_CYL)
		cylinders = be32_to_cpu(f4->DS4DCYL);

	DBG(LOWPROBE, ul_debug("DASD CDL: blk_per_trk=%u heads=%u cyl=%u blocksize=%u",
			blk_per_trk, heads, cylinders, blocksize));

	if (!heads || !blk_per_trk)
		return BLKID_PROBE_NONE;

	/*
	 * Scan blocks after F4 for Format 1 and Format 8 labels.
	 * These describe partitions. Maximum 3 partitions on DASD.
	 */
	for (blk = f4_blk + 1; blk < f4_blk + 20 && partno < DASD_MAX_PARTITIONS; blk++) {
		char dsnam[sizeof(f1->DS1DSNAM) + 1];
		uint32_t start_cc, end_cc;
		uint16_t start_hh, end_hh;
		uint64_t start_trk, end_trk;
		uint64_t start_512, size_512;
		blkid_partition par;

		buf = blkid_probe_get_buffer(pr,
				(uint64_t) blk * blocksize,
				sizeof(struct dasd_format1_label));
		if (!buf) {
			if (errno)
				return -errno;
			return BLKID_PROBE_NONE;
		}

		f1 = (const struct dasd_format1_label *) buf;

		if (f1->DS1FMTID != DASD_FMT_ID_F1 &&
		    f1->DS1FMTID != DASD_FMT_ID_F8)
			continue;

		/* Decode extent boundaries (CCHH) */
		if (cylinders > DASD_LV_COMPAT_CYL) {
			/* large volume encoding */
			start_cc = dasd_cchh_get_cc(&f1->DS1EXT1.llimit);
			start_hh = dasd_cchh_get_hh(&f1->DS1EXT1.llimit);
			end_cc = dasd_cchh_get_cc(&f1->DS1EXT1.ulimit);
			end_hh = dasd_cchh_get_hh(&f1->DS1EXT1.ulimit);
		} else {
			start_cc = be16_to_cpu(f1->DS1EXT1.llimit.cc);
			start_hh = be16_to_cpu(f1->DS1EXT1.llimit.hh);
			end_cc = be16_to_cpu(f1->DS1EXT1.ulimit.cc);
			end_hh = be16_to_cpu(f1->DS1EXT1.ulimit.hh);
		}

		start_trk = (uint64_t) start_cc * heads + start_hh;
		end_trk = (uint64_t) end_cc * heads + end_hh;

		/* Convert to 512-byte sectors */
		start_512 = start_trk * blk_per_trk * blocksize / 512;
		size_512 = (end_trk - start_trk + 1) * blk_per_trk * blocksize / 512;

		DBG(LOWPROBE, ul_debug("DASD CDL part%d: CC=%u-%u HH=%u-%u "
				"trk=%"PRIu64"-%"PRIu64" start=%"PRIu64" size=%"PRIu64,
				partno + 1, start_cc, end_cc, start_hh, end_hh,
				start_trk, end_trk, start_512, size_512));

		par = blkid_partlist_add_partition(ls, tab, start_512, size_512);
		if (!par)
			return -ENOMEM;

		dasd_get_dsnam(f1, dsnam);
		blkid_partition_set_type_string(par,
				(unsigned char *) dsnam, strlen(dsnam));

		partno++;
	}

	return BLKID_PROBE_OK;
}

/*
 * Parse LDL (LNX1/CMS1) partition table.
 *
 * LDL has a single implicit partition starting at block 3.
 */
static int probe_dasd_pt_ldl(blkid_probe pr, blkid_partlist ls,
			     blkid_parttable tab,
			     const struct dasd_volume_label *vlabel,
			     unsigned int blocksize)
{
	uint64_t start_512, size_512;
	uint64_t devsize;
	blkid_partition par;

	devsize = blkid_probe_get_size(pr);
	if (devsize == 0)
		return BLKID_PROBE_NONE;

	/* Partition starts at block 3 (after label and VTOC area) */
	start_512 = (uint64_t) 3 * blocksize / 512;

	if ((unsigned char) vlabel->ldl_version >= 0xf2) {
		/* Use formatted_blocks from label for exact size */
		uint64_t blocks = be64_to_cpu(vlabel->formatted_blocks);

		if (blocks > 3)
			size_512 = (blocks - 3) * blocksize / 512;
		else
			size_512 = devsize / 512 - start_512;
	} else {
		size_512 = devsize / 512 - start_512;
	}

	DBG(LOWPROBE, ul_debug("DASD LDL: start=%"PRIu64" size=%"PRIu64" blocksize=%u",
			start_512, size_512, blocksize));

	par = blkid_partlist_add_partition(ls, tab, start_512, size_512);
	if (!par)
		return -ENOMEM;

	return BLKID_PROBE_OK;
}

static int probe_dasd_pt(blkid_probe pr,
		const struct blkid_idmag *mag __attribute__((__unused__)))
{
	const struct dasd_volume_label *vlabel;
	const unsigned char *buf;
	blkid_parttable tab = NULL;
	blkid_partlist ls;
	char volser[DASD_VOLSER_LENGTH + 1];
	unsigned int blocksize;
	int is_cdl = 0, is_ldl = 0;
	int rc;

	blocksize = blkid_probe_get_sectorsize(pr);

	/*
	 * Read the volume label at block 2 (byte offset = 2 * blocksize).
	 */
	buf = blkid_probe_get_buffer(pr,
			(uint64_t) 2 * blocksize,
			sizeof(struct dasd_volume_label));
	if (!buf) {
		if (errno)
			return -errno;
		goto nothing;
	}

	vlabel = (const struct dasd_volume_label *) buf;

	/* CDL: "VOL1" at vollbl (offset 4) */
	if (memcmp(vlabel->vollbl, DASD_VOL1_MAGIC, 4) == 0)
		is_cdl = 1;
	/* LDL: "LNX1" or "CMS1" at volkey (offset 0) */
	else if (memcmp(vlabel->volkey, DASD_LNX1_MAGIC, 4) == 0 ||
		 memcmp(vlabel->volkey, DASD_CMS1_MAGIC, 4) == 0)
		is_ldl = 1;

	/*
	 * If sector size is < 4096 (e.g., probing a disk image with 512-byte
	 * sectors), retry with typical DASD block sizes.
	 */
	if (!is_cdl && !is_ldl && blocksize < 4096) {
		static const unsigned int try_sizes[] = { 4096, 2048, 1024 };
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(try_sizes); i++) {
			if (try_sizes[i] <= blocksize)
				continue;

			buf = blkid_probe_get_buffer(pr,
					(uint64_t) 2 * try_sizes[i],
					sizeof(struct dasd_volume_label));
			if (!buf)
				continue;

			vlabel = (const struct dasd_volume_label *) buf;

			if (memcmp(vlabel->vollbl, DASD_VOL1_MAGIC, 4) == 0) {
				is_cdl = 1;
				blocksize = try_sizes[i];
				break;
			}
			if (memcmp(vlabel->volkey, DASD_LNX1_MAGIC, 4) == 0 ||
			    memcmp(vlabel->volkey, DASD_CMS1_MAGIC, 4) == 0) {
				is_ldl = 1;
				blocksize = try_sizes[i];
				break;
			}
		}
	}

	if (!is_cdl && !is_ldl)
		goto nothing;

	DBG(LOWPROBE, ul_debug("DASD: %s label detected (blocksize=%u)",
			is_cdl ? "CDL" : "LDL", blocksize));

	/*
	 * Set magic for wipefs.  CDL has "VOL1" at vollbl (byte 4 of the
	 * label), LDL has "LNX1" or "CMS1" at volkey (byte 0).
	 */
	if (is_cdl) {
		if (blkid_probe_set_magic(pr,
				(uint64_t) 2 * blocksize + offsetof(struct dasd_volume_label, vollbl),
				4, (const unsigned char *) DASD_VOL1_MAGIC))
			goto nothing;
	} else {
		const char *magic = memcmp(vlabel->volkey, DASD_LNX1_MAGIC, 4) == 0 ?
					DASD_LNX1_MAGIC : DASD_CMS1_MAGIC;
		if (blkid_probe_set_magic(pr,
				(uint64_t) 2 * blocksize + offsetof(struct dasd_volume_label, volkey),
				4, (const unsigned char *) magic))
			goto nothing;
	}

	dasd_get_volser(vlabel, volser);

	blkid_partitions_strcpy_ptuuid(pr, volser);

	if (blkid_partitions_need_typeonly(pr))
		return BLKID_PROBE_OK;

	ls = blkid_probe_get_partlist(pr);
	if (!ls)
		goto nothing;

	tab = blkid_partlist_new_parttable(ls, "dasd", 0);
	if (!tab)
		return -ENOMEM;

	blkid_parttable_set_id(tab, (unsigned char *) volser);

	if (is_cdl)
		rc = probe_dasd_pt_cdl(pr, ls, tab, blocksize);
	else
		rc = probe_dasd_pt_ldl(pr, ls, tab, vlabel, blocksize);

	return rc;

nothing:
	return BLKID_PROBE_NONE;
}

const struct blkid_idinfo dasd_pt_idinfo =
{
	.name		= "dasd",
	.probefunc	= probe_dasd_pt,

	/*
	 * BLKID_NONE_MAGIC because the label offset depends on the variable
	 * blocksize and CDL/LDL have magic at different sub-offsets.
	 * The probefunc rejects non-DASD devices after one buffer read.
	 */
	.magics		= BLKID_NONE_MAGIC
};
