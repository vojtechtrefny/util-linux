/*
 * DASD (IBM s390/z-series) partition table probing
 *
 * Copyright (C) 2025 Red Hat, Inc. All rights reserved.
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#include "pt-dasd.h"
#include "partitions.h"

/*
 * Convert CCHH address to linear track number.
 * Uses the large-volume encoding which is backwards-compatible
 * with small volumes: cyl = ((hh & 0xFFF0) << 12) | cc, head = hh & 0x000F
 */
static uint32_t dasd_cchh2trk(const struct dasd_cchh *addr, uint16_t heads)
{
	uint16_t cc = be16_to_cpu(addr->cc);
	uint16_t hh = be16_to_cpu(addr->hh);
	uint32_t cyl;
	uint16_t head;

	cyl = (hh & 0xFFF0);
	cyl <<= 12;
	cyl |= cc;

	head = hh & 0x000F;

	return cyl * heads + head;
}

/* Minimal EBCDIC-to-ASCII conversion for volume serial and dataset names */
static char ebcdic_to_ascii(uint8_t c)
{
	/* A-I: 0xC1-0xC9 -> 0x41-0x49 */
	if (c >= 0xC1 && c <= 0xC9)
		return 'A' + (c - 0xC1);
	/* J-R: 0xD1-0xD9 -> 0x4A-0x52 */
	if (c >= 0xD1 && c <= 0xD9)
		return 'J' + (c - 0xD1);
	/* S-Z: 0xE2-0xE9 -> 0x53-0x5A */
	if (c >= 0xE2 && c <= 0xE9)
		return 'S' + (c - 0xE2);
	/* 0-9: 0xF0-0xF9 -> 0x30-0x39 */
	if (c >= 0xF0 && c <= 0xF9)
		return '0' + (c - 0xF0);
	/* Space: 0x40 -> 0x20 */
	if (c == 0x40)
		return ' ';
	/* Period: 0x4B -> 0x2E */
	if (c == 0x4B)
		return '.';
	/* Slash: 0x61 -> 0x2F */
	if (c == 0x61)
		return '/';

	return ' ';
}

static void ebcdic_to_ascii_buf(const unsigned char *src, unsigned char *dst,
				size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		dst[i] = ebcdic_to_ascii(src[i]);
	dst[len] = '\0';
}

static void set_ptuuid(blkid_probe pr, const struct dasd_volume_label *vlabel)
{
	char volser[DASD_VOLSER_LENGTH + 1];
	int i;

	for (i = 0; i < DASD_VOLSER_LENGTH; i++)
		volser[i] = ebcdic_to_ascii((unsigned char) vlabel->volid[i]);
	volser[DASD_VOLSER_LENGTH] = '\0';

	/* trim trailing spaces */
	for (i = DASD_VOLSER_LENGTH - 1; i >= 0 && volser[i] == ' '; i--)
		volser[i] = '\0';

	blkid_partitions_strcpy_ptuuid(pr, volser);
}

/*
 * Validate a Format 4 label: the key area must be 44 bytes of 0x04
 * followed by the format identifier 0xF4.
 */
static int is_valid_f4(const struct dasd_format4_label *f4)
{
	size_t i;

	for (i = 0; i < sizeof(f4->DS4KEYCD); i++) {
		if (f4->DS4KEYCD[i] != 0x04)
			return 0;
	}

	return f4->DS4IDFMT == DASD_FMT_ID_F4;
}

/*
 * LDL (Linux Disk Layout) and CMS disks have no partition table --
 * report an empty table like the AIX prober does.
 */
static int probe_dasd_ldl(blkid_probe pr)
{
	blkid_partlist ls;
	blkid_parttable tab;

	if (blkid_partitions_need_typeonly(pr))
		return BLKID_PROBE_OK;

	ls = blkid_probe_get_partlist(pr);
	if (!ls)
		return BLKID_PROBE_NONE;

	tab = blkid_partlist_new_parttable(ls, "dasd", 0);
	if (!tab)
		return -ENOMEM;

	return BLKID_PROBE_OK;
}

/*
 * Known sectors-per-track values for DASD devices. The most common are
 * 12 (3390 with 4096 byte blocks) and 15 (3390 with smaller blocks).
 * We try each candidate and validate against the F4 label signature.
 */
static const unsigned int dasd_spt_candidates[] = {
	12, 15, 7, 8, 9, 10, 24, 48
};

static int probe_dasd_pt(blkid_probe pr,
		const struct blkid_idmag *mag)
{
	const struct dasd_volume_label *vlabel;
	const struct dasd_format4_label *f4;
	const struct dasd_format1_label *f1;
	blkid_parttable tab = NULL;
	blkid_partlist ls;
	unsigned int blocksize;
	uint64_t vtoc_offset = 0;
	unsigned int spt = 0;
	uint16_t heads;
	unsigned int nparts = 0;
	size_t i;

	/* hint == 0 means LDL or CMS (magic at offset 0) */
	if (mag->hint == 0)
		return probe_dasd_ldl(pr);

	/* CDL: hint carries the blocksize */
	blocksize = mag->hint;

	/* Read and verify the volume label at block 2 */
	vlabel = (const struct dasd_volume_label *)
		blkid_probe_get_buffer(pr,
			(uint64_t) 2 * blocksize,
			sizeof(*vlabel));
	if (!vlabel) {
		if (errno)
			return -errno;
		goto nothing;
	}

	if (memcmp(vlabel->vollbl, DASD_VOL1_MAGIC, 4) != 0)
		goto nothing;

	/*
	 * Find the VTOC (Format 4 label). The VTOC is at track 1 (CC=0, HH=1),
	 * but since we don't know the sectors-per-track (spt) value, we try
	 * known candidates and validate by checking for the F4 signature.
	 */
	for (i = 0; i < ARRAY_SIZE(dasd_spt_candidates); i++) {
		unsigned int try_spt = dasd_spt_candidates[i];
		uint64_t off = (uint64_t) try_spt * blocksize;

		f4 = (const struct dasd_format4_label *)
			blkid_probe_get_buffer(pr, off, sizeof(*f4));
		if (!f4) {
			if (errno)
				return -errno;
			continue;
		}

		if (is_valid_f4(f4)) {
			spt = try_spt;
			vtoc_offset = off;
			break;
		}
	}

	if (spt == 0) {
		DBG(LOWPROBE, ul_debug("DASD: F4 label not found"));
		goto nothing;
	}

	DBG(LOWPROBE, ul_debug("DASD: F4 found at offset %ju, spt=%u, blocksize=%u",
				(uintmax_t) vtoc_offset, spt, blocksize));

	heads = be16_to_cpu(f4->DS4DEVCT.DS4DSTRK);
	if (heads == 0)
		goto nothing;

	// /* Convert volume serial from EBCDIC to ASCII */
	// {
	// 	unsigned char volser[7];

	// 	ebcdic_to_ascii_buf((const unsigned char *) vlabel->volid,
	// 			    volser, sizeof(vlabel->volid));
	// 	blkid_rtrim_whitespace(volser);
	// 	blkid_partitions_strcpy_ptuuid(pr, (const char *) volser);
	// }
	set_ptuuid(pr, vlabel);


	if (blkid_partitions_need_typeonly(pr))
		return BLKID_PROBE_OK;

	ls = blkid_probe_get_partlist(pr);
	if (!ls)
		goto nothing;

	tab = blkid_partlist_new_parttable(ls, "dasd", vtoc_offset);
	if (!tab)
		goto err;

	/*
	 * Enumerate partition entries. VTOC labels are stored one per block,
	 * spaced blocksize bytes apart, starting after the F4 label.
	 */
	for (i = 1; i < spt && nparts < DASD_MAX_PARTITIONS; i++) {
		uint64_t off = vtoc_offset + (uint64_t) i * blocksize;
		uint32_t start_trk, end_trk;
		uint64_t start_sector, size_sectors;
		blkid_partition par;

		f1 = (const struct dasd_format1_label *)
			blkid_probe_get_buffer(pr, off, sizeof(*f1));
		if (!f1) {
			if (errno)
				return -errno;
			break;
		}

		switch (f1->DS1FMTID) {
		case DASD_FMT_ID_F1:
		case DASD_FMT_ID_F8:
			/* Partition entry -- extract extent */
			start_trk = dasd_cchh2trk(&f1->DS1EXT1.llimit, heads);
			end_trk = dasd_cchh2trk(&f1->DS1EXT1.ulimit, heads);

			/* Convert tracks to 512-byte sectors */
			start_sector = (uint64_t) start_trk * spt
				       * (blocksize / 512);
			size_sectors = (uint64_t) (end_trk - start_trk + 1)
				       * spt * (blocksize / 512);

			par = blkid_partlist_add_partition(ls, tab,
					start_sector, size_sectors);
			if (!par)
				goto err;

			/* Set partition name from EBCDIC dataset name */
			{
				unsigned char name[45];

				ebcdic_to_ascii_buf(
					(const unsigned char *) f1->DS1DSNAM,
					name, sizeof(f1->DS1DSNAM));
				blkid_partition_set_name(par, name,
						sizeof(f1->DS1DSNAM));
			}

			nparts++;
			break;

		case DASD_FMT_ID_F5:
		case DASD_FMT_ID_F7:
		case DASD_FMT_ID_F9:
			/* Free space / large volume free space / associated
			 * format 9 labels -- skip */
			break;

		default:
			/* Unknown or end of VTOC entries */
			if (f1->DS1FMTID == 0)
				goto done;
			break;
		}
	}

done:
	return BLKID_PROBE_OK;

nothing:
	return BLKID_PROBE_NONE;
err:
	return -ENOMEM;
}

/*
 * DASD partition table identification.
 *
 * CDL (Compatible Disk Layout): "VOL1" EBCDIC magic at block 2. We probe
 * for each known blocksize (512, 1024, 2048, 4096); the hint carries the
 * blocksize to the probe function.
 *
 * LDL (Linux Disk Layout): "LNX1" EBCDIC magic at offset 0.
 * CMS: "CMS1" EBCDIC magic at offset 0.
 * For LDL/CMS the hint is 0, indicating whole-disk (no partitions).
 */
const struct blkid_idinfo dasd_pt_idinfo =
{
	.name		= "dasd",
	.probefunc	= probe_dasd_pt,
	.magics		=
	{
		/* CDL: "VOL1" at block 2, for each blocksize */
		{ .magic = DASD_VOL1_MAGIC, .len = 4, .kboff = 1,
		  .hint = 512 },
		{ .magic = DASD_VOL1_MAGIC, .len = 4, .kboff = 2,
		  .hint = 1024 },
		{ .magic = DASD_VOL1_MAGIC, .len = 4, .kboff = 4,
		  .hint = 2048 },
		{ .magic = DASD_VOL1_MAGIC, .len = 4, .kboff = 8,
		  .hint = 4096 },
		/* LDL: "LNX1" at offset 0 */
		{ .magic = DASD_LNX1_MAGIC, .len = 4, .kboff = 0,
		  .hint = 0 },
		/* CMS: "CMS1" at offset 0 */
		{ .magic = DASD_CMS1_MAGIC, .len = 4, .kboff = 0,
		  .hint = 0 },
		{ NULL }
	}
};
