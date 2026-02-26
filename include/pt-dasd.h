/*
 * No copyright is claimed.  This code is in the public domain; do with
 * it what you wish.
 *
 * DASD (IBM s390/z-series) partition table structures
 */
#ifndef UTIL_LINUX_PT_DASD_H
#define UTIL_LINUX_PT_DASD_H

#include <stdint.h>

/* Volume label magic strings (EBCDIC) */
#define DASD_VOL1_MAGIC		"\xE5\xD6\xD3\xF1"	/* "VOL1" */
#define DASD_LNX1_MAGIC		"\xD3\xD5\xE7\xF1"	/* "LNX1" */
#define DASD_CMS1_MAGIC		"\xC3\xD4\xE2\xF1"	/* "CMS1" */

/* Format label identifiers */
#define DASD_FMT_ID_F1		0xF1
#define DASD_FMT_ID_F4		0xF4
#define DASD_FMT_ID_F5		0xF5
#define DASD_FMT_ID_F7		0xF7
#define DASD_FMT_ID_F8		0xF8
#define DASD_FMT_ID_F9		0xF9

#define DASD_MAX_PARTITIONS	3
#define DASD_VOLSER_LENGTH	6

struct dasd_cchh {
	uint16_t cc;
	uint16_t hh;
} __attribute__ ((packed));

struct dasd_cchhb {
	uint16_t cc;
	uint16_t hh;
	uint8_t b;
} __attribute__ ((packed));

struct dasd_extent {
	uint8_t typeind;
	uint8_t seqno;
	struct dasd_cchh llimit;
	struct dasd_cchh ulimit;
} __attribute__ ((packed));

struct dasd_volume_label {
	char volkey[4];			/* key ("VOL1" EBCDIC for CDL) */
	char vollbl[4];			/* label identifier */
	char volid[6];			/* volume serial */
	uint8_t security;		/* security byte */
	struct dasd_cchhb vtoc;		/* VTOC address */
	char res1[5];			/* reserved */
	char cisize[4];			/* CI-size (FBA), blanks for CKD */
	char blkperci[4];		/* blocks per CI (FBA) */
	char labperci[4];		/* labels per CI (FBA) */
	char res2[4];			/* reserved */
	char lvtoc[14];			/* owner code for LVTOC */
	char res3[28];			/* reserved */
} __attribute__ ((packed));

struct dasd_dev_const {
	uint16_t DS4DSCYL;		/* number of logical cylinders */
	uint16_t DS4DSTRK;		/* number of tracks per cylinder */
	uint16_t DS4DEVTK;		/* device track length */
	uint8_t DS4DEVI;		/* non-last keyed record overhead */
	uint8_t DS4DEVL;		/* last keyed record overhead */
	uint8_t DS4DEVK;		/* non-keyed record overhead */
	uint8_t DS4DEVFG;		/* flag byte */
	uint16_t DS4DEVTL;		/* device tolerance */
	uint8_t DS4DEVDT;		/* DSCBs per track */
	uint8_t DS4DEVDB;		/* directory blocks per track */
} __attribute__ ((packed));

struct dasd_format4_label {
	char DS4KEYCD[44];		/* key: 44 bytes of 0x04 */
	uint8_t DS4IDFMT;		/* format identifier (0xF4) */
	struct dasd_cchhb DS4HPCHR;	/* highest F1 DSCB address */
	uint16_t DS4DSREC;		/* available DSCBs */
	struct dasd_cchh DS4HCCHH;	/* next alt track address */
	uint16_t DS4NOATK;		/* remaining alt tracks */
	uint8_t DS4VTOCI;		/* VTOC indicators */
	uint8_t DS4NOEXT;		/* number of extents in VTOC */
	uint8_t DS4SMSFG;		/* SMS indicators */
	uint8_t DS4DEVAC;		/* alternate cylinders */
	struct dasd_dev_const DS4DEVCT;	/* device constants */
	char DS4AMTIM[8];		/* VSAM timestamp */
	char DS4AMCAT[3];		/* VSAM catalog indicator */
	char DS4R2TIM[8];		/* VSAM volume/catalog timestamp */
	char res1[5];			/* reserved */
	char DS4F6PTR[5];		/* pointer to first F6 DSCB */
	struct dasd_extent DS4VTOCE;	/* VTOC extent description */
	char res2[10];			/* reserved */
	uint8_t DS4EFLVL;		/* extended free-space level */
	struct dasd_cchhb DS4EFPTR;	/* extended free-space pointer */
	char res3;			/* reserved */
	uint32_t DS4DCYL;		/* logical cylinders (large) */
	char res4[2];			/* reserved */
	uint8_t DS4DEVF2;		/* device flags */
	char res5;			/* reserved */
} __attribute__ ((packed));

/* Format 1 and format 8 labels have the same layout */
struct dasd_format1_label {
	char DS1DSNAM[44];		/* data set name (EBCDIC) */
	uint8_t DS1FMTID;		/* format identifier */
	char DS1DSSN[6];		/* data set serial number */
	uint16_t DS1VOLSQ;		/* volume sequence number */
	char DS1CREDT[3];		/* creation date */
	char DS1EXPDT[3];		/* expiration date */
	uint8_t DS1NOEPV;		/* number of extents on volume */
	uint8_t DS1NOBDB;		/* bytes used in last dir block */
	uint8_t DS1FLAG1;		/* flag 1 */
	char DS1SYSCD[13];		/* system code */
	char DS1REFD[3];		/* date last referenced */
	uint8_t DS1SMSFG;		/* SMS indicators */
	uint8_t DS1SCXTF;		/* sec. space extension flag */
	uint16_t DS1SCXTV;		/* secondary space extension */
	uint8_t DS1DSRG1;		/* data set organization byte 1 */
	uint8_t DS1DSRG2;		/* data set organization byte 2 */
	uint8_t DS1RECFM;		/* record format */
	uint8_t DS1OPTCD;		/* option code */
	uint16_t DS1BLKL;		/* block length */
	uint16_t DS1LRECL;		/* record length */
	uint8_t DS1KEYL;		/* key length */
	uint16_t DS1RKP;		/* relative key position */
	uint8_t DS1DSIND;		/* data set indicators */
	uint8_t DS1SCAL1;		/* secondary allocation flag */
	char DS1SCAL3[3];		/* secondary allocation quantity */
	char DS1LSTAR[3];		/* last used track and block */
	uint16_t DS1TRBAL;		/* space remaining on last track */
	uint16_t res1;			/* reserved */
	struct dasd_extent DS1EXT1;	/* first extent description */
	struct dasd_extent DS1EXT2;	/* second extent description */
	struct dasd_extent DS1EXT3;	/* third extent description */
	struct dasd_cchhb DS1PTRDS;	/* pointer to F2 or F3 DSCB */
} __attribute__ ((packed));

#endif /* UTIL_LINUX_PT_DASD_H */
