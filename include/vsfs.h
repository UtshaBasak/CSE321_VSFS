/*
 * vsfs.h - On-disk format definition for the Very Simple File System (VSFS).
 *
 * This header is the single source of truth for the layout shared by every
 * tool in this project (mkfs, validator, journal). Any change to a constant or
 * structure here changes the on-disk format, so all three tools must be
 * rebuilt together.
 *
 * Block layout (85 blocks x 4096 bytes = 348,160 bytes):
 *
 *   Block  0        Superblock
 *   Blocks 1 - 16   Journal region (16 blocks)
 *   Block  17       Inode bitmap
 *   Block  18       Data bitmap
 *   Blocks 19 - 20  Inode table (2 blocks, 64 inodes)
 *   Blocks 21 - 84  Data region (64 blocks)
 *
 * All multi-byte fields are stored in the host byte order of the machine that
 * created the image; images are not portable across endianness.
 */

#ifndef VSFS_H
#define VSFS_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

#define FS_MAGIC          0x56534653U   /* "VSFS" */
#define BLOCK_SIZE              4096U
#define INODE_SIZE               128U

#define JOURNAL_BLOCK_IDX          1U
#define JOURNAL_BLOCKS            16U
#define INODE_BLOCKS               2U
#define DATA_BLOCKS               64U

#define INODE_BMAP_IDX    (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX     (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX   (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX    (INODE_START_IDX + INODE_BLOCKS)
#define TOTAL_BLOCKS      (DATA_START_IDX + DATA_BLOCKS)

#define INODES_PER_BLOCK  (BLOCK_SIZE / INODE_SIZE)
#define INODE_COUNT       (INODE_BLOCKS * INODES_PER_BLOCK)

#define DIRECT_POINTERS            8U
#define NAME_LEN                   28U

#define DEFAULT_IMAGE     "vsfs.img"

/* Inode type values. */
#define INODE_FREE                 0U
#define INODE_FILE                 1U
#define INODE_DIR                  2U

/* ------------------------------------------------------------------ */
/* On-disk structures                                                  */
/* ------------------------------------------------------------------ */

struct superblock {
    uint32_t magic;          /* FS_MAGIC                                */
    uint32_t block_size;     /* BLOCK_SIZE                              */
    uint32_t total_blocks;   /* TOTAL_BLOCKS                            */
    uint32_t inode_count;    /* INODE_COUNT                             */
    uint32_t journal_block;  /* first block of the journal region       */
    uint32_t inode_bitmap;   /* block holding the inode bitmap          */
    uint32_t data_bitmap;    /* block holding the data bitmap           */
    uint32_t inode_start;    /* first block of the inode table          */
    uint32_t data_start;     /* first block of the data region          */
    uint8_t  _pad[128 - 9 * 4];
} __attribute__((packed));

struct inode {
    uint16_t type;           /* INODE_FREE / INODE_FILE / INODE_DIR     */
    uint16_t links;          /* number of directory entries pointing here */
    uint32_t size;           /* size in bytes                           */
    uint32_t direct[DIRECT_POINTERS]; /* absolute block numbers, 0 = unused */
    uint32_t ctime;          /* creation time, seconds since the epoch  */
    uint32_t mtime;          /* last modification time                  */
    uint8_t  _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];
} __attribute__((packed));

struct dirent {
    uint32_t inode;          /* 0 together with an empty name = free slot */
    char     name[NAME_LEN]; /* NUL-terminated                          */
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* Journal format                                                      */
/* ------------------------------------------------------------------ */

#define JOURNAL_MAGIC     0x4A524E4CU   /* "JRNL" */

#define REC_DATA              0x0001U   /* block image to be installed   */
#define REC_COMMIT            0x0002U   /* makes preceding records durable */

/* Stored at the very start of the journal region (block JOURNAL_BLOCK_IDX). */
struct journal_header {
    uint32_t magic;          /* JOURNAL_MAGIC                           */
    uint32_t nbytes_used;    /* write offset of the next record         */
} __attribute__((packed));

/* Every journal record begins with this header. */
struct rec_header {
    uint16_t type;           /* REC_DATA or REC_COMMIT                  */
    uint16_t size;           /* total record size in bytes              */
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* Layout guarantees                                                   */
/* ------------------------------------------------------------------ */

_Static_assert(sizeof(struct superblock) == 128, "superblock must be 128 bytes");
_Static_assert(sizeof(struct inode) == INODE_SIZE, "inode must be 128 bytes");
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");
_Static_assert(sizeof(struct journal_header) == 8, "journal header must be 8 bytes");
_Static_assert(sizeof(struct rec_header) == 4, "record header must be 4 bytes");

#endif /* VSFS_H */
