/*
 * journal.c - Write-ahead journaling layer for VSFS.
 *
 * Implements two commands:
 *
 *   create <name>   Stage the creation of a file in the root directory as a
 *                   single transaction. Nothing is written to the filesystem
 *                   proper; the new inode bitmap, inode table block and root
 *                   directory block are appended to the journal and sealed
 *                   with a commit record.
 *
 *   install         Replay the journal. Data records belonging to a committed
 *                   transaction are written to their home blocks; records
 *                   after the last commit are discarded. The journal is then
 *                   cleared.
 *
 * Because create() never touches the filesystem directly, a crash before the
 * commit record leaves the image exactly as it was, and a crash after it
 * leaves a transaction that the next install replays in full.
 *
 * Usage: journal create <name>
 *        journal install
 *
 * Both commands operate on vsfs.img in the current directory.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vsfs.h"

/* Helper I/O functions */
static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void pread_block(int fd, uint32_t block_index, void *buf) {
    off_t offset = (off_t)block_index * (off_t)BLOCK_SIZE;
    ssize_t n = pread(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE)
        die("pread_block");
}

static void pwrite_block(int fd, uint32_t block_index, const void *buf) {
    off_t offset = (off_t)block_index * (off_t)BLOCK_SIZE;
    ssize_t n = pwrite(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE)
        die("pwrite_block");
}

static void pread_bytes(int fd, off_t offset, void *buf, size_t size) {
    ssize_t n = pread(fd, buf, size, offset);
    if (n != (ssize_t)size)
        die("pread_bytes");
}

static void pwrite_bytes(int fd, off_t offset, const void *buf, size_t size) {
    ssize_t n = pwrite(fd, buf, size, offset);
    if (n != (ssize_t)size)
        die("pwrite_bytes");
}

static int bitmap_test(const uint8_t *bitmap, uint32_t index) {
    return (bitmap[index / 8] >> (index % 8)) & 0x1;
}

static void bitmap_set(uint8_t *bitmap, uint32_t index) {
    bitmap[index / 8] |= (uint8_t)(1U << (index % 8));
}

static void journal_read_header(int fd, struct journal_header *jh) {
    pread_bytes(fd, (off_t)JOURNAL_BLOCK_IDX * (off_t)BLOCK_SIZE, jh, sizeof(*jh));
}

/* Only writing the first 8 bytes (journal_header) */
static void journal_write_header(int fd, const struct journal_header *jh) {
    pwrite_bytes(fd, (off_t)JOURNAL_BLOCK_IDX * (off_t)BLOCK_SIZE, jh, sizeof(*jh));
}

static void journal_init_if_needed(int fd) {
    struct journal_header jh;
    journal_read_header(fd, &jh);

    if (jh.magic != JOURNAL_MAGIC) {
        jh.magic = JOURNAL_MAGIC;
        jh.nbytes_used = sizeof(struct journal_header); /* empty journal */
        journal_write_header(fd, &jh);
    }
}

/* Total journal capacity in bytes (16 blocks) */
static uint32_t journal_capacity_bytes(void) {
    return JOURNAL_BLOCKS * BLOCK_SIZE;
}

/* data record size = rec_header (4) + block_no (4) + 4096 bytes full block image */
static uint32_t data_record_size(void) {
    return (uint32_t)(sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE);
}

static uint32_t commit_record_size(void) {
    return (uint32_t)sizeof(struct rec_header);
}

/* Appending one data record at the current end of the journal */
static void journal_append_data(int fd, struct journal_header *jh, uint32_t block_no, const uint8_t *block_img) {
    struct rec_header rh;
    rh.type = REC_DATA;
    rh.size = (uint16_t)data_record_size();

    off_t journal_start = (off_t)JOURNAL_BLOCK_IDX * (off_t)BLOCK_SIZE;
    off_t off = journal_start + (off_t)jh->nbytes_used;

    pwrite_bytes(fd, off, &rh, sizeof(rh));
    off += (off_t)sizeof(rh);

    pwrite_bytes(fd, off, &block_no, sizeof(block_no));
    off += (off_t)sizeof(block_no);

    pwrite_bytes(fd, off, block_img, BLOCK_SIZE);

    jh->nbytes_used += rh.size;
}

/* Install only applies on data records that belong to a committed transaction */
static void journal_append_commit(int fd, struct journal_header *jh) {
    struct rec_header rh;
    rh.type = REC_COMMIT;
    rh.size = (uint16_t)commit_record_size();

    off_t journal_start = (off_t)JOURNAL_BLOCK_IDX * (off_t)BLOCK_SIZE;
    off_t off = journal_start + (off_t)jh->nbytes_used;

    pwrite_bytes(fd, off, &rh, sizeof(rh));
    jh->nbytes_used += rh.size;
}

/* Finding first 0-bit in inode bitmap. inode 0 is root so starting from 1. */
static int find_free_inode(const uint8_t *inode_bitmap, uint32_t inode_count) {
    for (uint32_t i = 1; i < inode_count; i++) {
        if (!bitmap_test(inode_bitmap, i))
            return (int)i;
    }
    return -1;
}

/* Finding a free directory-entry slot in an in-memory directory block */
static int find_free_dirent_slot_in_block(const uint8_t dir_block[BLOCK_SIZE]) {
    const struct dirent *ents = (const struct dirent *)dir_block;
    uint32_t n = BLOCK_SIZE / (uint32_t)sizeof(struct dirent);

    for (uint32_t i = 0; i < n; i++) {
        if (ents[i].inode == 0 && ents[i].name[0] == '\0')
            return (int)i;
    }
    return -1;
}

static int dirent_name_exists_in_block(const uint8_t dir_block[BLOCK_SIZE], const char *name) {
    const struct dirent *ents = (const struct dirent *)dir_block;
    uint32_t n = BLOCK_SIZE / (uint32_t)sizeof(struct dirent);

    for (uint32_t i = 0; i < n; i++) {
        if (ents[i].name[0] == '\0')
            continue;
        if (strncmp(ents[i].name, name, NAME_LEN) == 0)
            return 1;
    }
    return 0;
}

static void overlay_committed_journal_into_create_view(
    int fd,
    const struct journal_header *jh,
    uint8_t inode_bitmap[BLOCK_SIZE],
    uint8_t inode_area[INODE_BLOCKS * BLOCK_SIZE],
    uint32_t root_dirblk,
    uint8_t root_dir_block[BLOCK_SIZE]) {

    off_t journal_start = (off_t)JOURNAL_BLOCK_IDX * (off_t)BLOCK_SIZE;
    off_t offset = journal_start + (off_t)sizeof(struct journal_header);
    off_t journal_end = journal_start + (off_t)jh->nbytes_used;

    /* Pending images for the current transaction which will only apply at commit */
    int has_ibmap = 0;
    uint8_t pend_ibmap[BLOCK_SIZE];

    int has_ino[INODE_BLOCKS];
    uint8_t pend_ino[INODE_BLOCKS][BLOCK_SIZE];
    for (uint32_t i = 0; i < INODE_BLOCKS; i++)
        has_ino[i] = 0;

    int has_root = 0;
    uint8_t pend_root[BLOCK_SIZE];

    while (offset + (off_t)sizeof(struct rec_header) <= journal_end) {
        struct rec_header rh;
        pread_bytes(fd, offset, &rh, sizeof(rh));

        if (rh.size < sizeof(struct rec_header))
            break;
        if (offset + (off_t)rh.size > journal_end)
            break;

        off_t payload = offset + (off_t)sizeof(struct rec_header);

        if (rh.type == REC_DATA) {
            uint16_t expected = (uint16_t)data_record_size();
            if (rh.size != expected)
                break;

            uint32_t block_no;
            uint8_t img[BLOCK_SIZE];

            pread_bytes(fd, payload, &block_no, sizeof(block_no));
            pread_bytes(fd, payload + (off_t)sizeof(block_no), img, BLOCK_SIZE);

            /* tracking only the blocks that create wants for allocation */
            if (block_no == INODE_BMAP_IDX) {
                memcpy(pend_ibmap, img, BLOCK_SIZE);
                has_ibmap = 1;
            } else if (block_no >= INODE_START_IDX && block_no < INODE_START_IDX + INODE_BLOCKS) {
                uint32_t which = block_no - INODE_START_IDX;
                if (which >= INODE_BLOCKS) {
                    fprintf(stderr, "ERROR: Invalid inode block index %u\n", which);
                    break;
                }
                memcpy(pend_ino[which], img, BLOCK_SIZE);
                has_ino[which] = 1;
            } else if (block_no == root_dirblk) {
                memcpy(pend_root, img, BLOCK_SIZE);
                has_root = 1;
            }

        } else if (rh.type == REC_COMMIT) {
            if (rh.size != (uint16_t)commit_record_size())
                break;

            /* Applying committed transaction to our in-memory view */
            if (has_ibmap)
                memcpy(inode_bitmap, pend_ibmap, BLOCK_SIZE);

            for (uint32_t i = 0; i < INODE_BLOCKS; i++) {
                if (has_ino[i])
                    memcpy(inode_area + (i * BLOCK_SIZE), pend_ino[i], BLOCK_SIZE);
            }

            if (has_root)
                memcpy(root_dir_block, pend_root, BLOCK_SIZE);

            /* Resetting pending transaction state for next transaction */
            has_ibmap = 0;
            for (uint32_t i = 0; i < INODE_BLOCKS; i++)
                has_ino[i] = 0;
            has_root = 0;

        } else {
            break;
        }

        offset += (off_t)rh.size;
    }
}

/* create command */
static void cmd_create(const char *image_path, const char *name) {
    int fd = open(image_path, O_RDWR);
    if (fd < 0)
        die("open");

    journal_init_if_needed(fd);

    /* Read superblock */
    uint8_t sb_block[BLOCK_SIZE];
    pread_block(fd, 0, sb_block);
    struct superblock *sb = (struct superblock *)sb_block;

    if (sb->magic != FS_MAGIC || sb->block_size != BLOCK_SIZE) {
        fprintf(stderr, "ERROR: invalid filesystem image.\n");
        close(fd);
        exit(1);
    }

    /* inode_count bounds both the free-inode scan and the index into the
       fixed-size inode table buffer below, so reject an out-of-range value
       instead of trusting the image. */
    if (sb->inode_count == 0 || sb->inode_count > INODE_COUNT) {
        fprintf(stderr, "ERROR: superblock reports %u inodes (max %u).\n",
                sb->inode_count, (unsigned)INODE_COUNT);
        close(fd);
        exit(1);
    }

    if (strlen(name) >= NAME_LEN) {
        fprintf(stderr, "ERROR: Filename too long (max %u chars).\n", NAME_LEN - 1);
        close(fd);
        exit(1);
    }

    uint8_t inode_bitmap[BLOCK_SIZE];
    pread_block(fd, INODE_BMAP_IDX, inode_bitmap);

    uint8_t inode_area[INODE_BLOCKS * BLOCK_SIZE];
    for (uint32_t i = 0; i < INODE_BLOCKS; i++) {
        pread_block(fd, INODE_START_IDX + i, inode_area + (i * BLOCK_SIZE));
    }
    struct inode *inodes = (struct inode *)inode_area;

    uint32_t root_dirblk = inodes[0].direct[0];

    uint8_t root_dir_block[BLOCK_SIZE];
    pread_block(fd, root_dirblk, root_dir_block);

    struct journal_header jh;
    journal_read_header(fd, &jh);

    if (jh.magic != JOURNAL_MAGIC) {
        fprintf(stderr, "ERROR: journal not initialized.\n");
        close(fd);
        exit(1);
    }

    overlay_committed_journal_into_create_view(fd, &jh, inode_bitmap, inode_area, root_dirblk, root_dir_block);
    inodes = (struct inode *)inode_area;

    if (dirent_name_exists_in_block(root_dir_block, name)) {
        fprintf(stderr, "ERROR: File already exists.\n");
        close(fd);
        exit(1);
    }

    /* assigning new inode number based on overlayed in-memory view */
    int new_inum = find_free_inode(inode_bitmap, sb->inode_count);
    if (new_inum < 0) {
        fprintf(stderr, "ERROR: No free inodes available.\n");
        close(fd);
        exit(1);
    }

    int slot = find_free_dirent_slot_in_block(root_dir_block);
    if (slot < 0) {
        fprintf(stderr, "ERROR: Root directory is full.\n");
        close(fd);
        exit(1);
    }

    uint32_t inode_block_offset = (uint32_t)new_inum / INODES_PER_BLOCK; /* 0..INODE_BLOCKS-1 */

    /* Ensuring that there is space for a full transaction (ibmap + one inode block + root dir) */
    uint32_t n_data_records = 1U + 1U + 1U; /* ibmap + one inode block + root dir */
    uint32_t need = n_data_records * data_record_size() + commit_record_size();
    uint32_t cap = journal_capacity_bytes();

    if (jh.nbytes_used + need > cap) {
        fprintf(stderr, "ERROR: Journal full. Run './journal install' first.\n");
        close(fd);
        exit(1);
    }

    /* Computing updated metadata blocks in memory */
    uint8_t new_inode_bitmap[BLOCK_SIZE];
    memcpy(new_inode_bitmap, inode_bitmap, BLOCK_SIZE);
    bitmap_set(new_inode_bitmap, (uint32_t)new_inum);

    uint8_t new_inode_area[INODE_BLOCKS * BLOCK_SIZE];
    memcpy(new_inode_area, inode_area, sizeof(new_inode_area));
    struct inode *new_inodes = (struct inode *)new_inode_area;

    struct inode *finode = &new_inodes[new_inum];
    memset(finode, 0, sizeof(*finode));
    finode->type = INODE_FILE;
    finode->links = 1;
    finode->size = 0;
    time_t now = time(NULL);
    finode->ctime = (uint32_t)now;
    finode->mtime = finode->ctime;

    struct inode *root = &new_inodes[0];
    uint32_t need_size = (uint32_t)((slot + 1) * sizeof(struct dirent));
    if (root->size < need_size)
        root->size = need_size;
    root->mtime = (uint32_t)now;

    uint8_t new_root_dir_block[BLOCK_SIZE];
    memcpy(new_root_dir_block, root_dir_block, BLOCK_SIZE);

    struct dirent *ents = (struct dirent *)new_root_dir_block;
    ents[slot].inode = (uint32_t)new_inum;
    memset(ents[slot].name, 0, NAME_LEN);
    strncpy(ents[slot].name, name, NAME_LEN - 1);
    ents[slot].name[NAME_LEN - 1] = '\0';

    journal_append_data(fd, &jh, INODE_BMAP_IDX, new_inode_bitmap);

    /* Only append the inode block that contains the newly allocated inode */
    journal_append_data(fd, &jh, INODE_START_IDX + inode_block_offset,
                        new_inode_area + (inode_block_offset * BLOCK_SIZE));

    journal_append_data(fd, &jh, root_dirblk, new_root_dir_block);

    journal_append_commit(fd, &jh);
    journal_write_header(fd, &jh);

    printf("File '%s' created (inode %d).\n", name, new_inum);
    close(fd);
}

/* install command that parses journal sequentially, applies data records
   only for transactions that have a commit, and clears journal after replay. */
static void cmd_install(const char *image_path) {
    int fd = open(image_path, O_RDWR);
    if (fd < 0)
        die("open");

    struct journal_header jh;
    journal_read_header(fd, &jh);

    if (jh.magic != JOURNAL_MAGIC) {
        fprintf(stderr, "ERROR: Journal does not exist.\n");
        close(fd);
        exit(1);
    }

    const uint32_t cap = journal_capacity_bytes();
    if (jh.nbytes_used < sizeof(struct journal_header) || jh.nbytes_used > cap) {
        fprintf(stderr, "ERROR: Journal header invalid (nbytes_used=%u).\n", jh.nbytes_used);
        close(fd);
        exit(1);
    }

    if (jh.nbytes_used == sizeof(struct journal_header)) {
        printf("Journal is empty. Nothing to install.\n");
        close(fd);
        return;
    }

    typedef struct {
        uint32_t block_no;
        uint8_t data[BLOCK_SIZE];
    } pending_write_t;

    pending_write_t *pending = NULL;
    size_t pending_cnt = 0;
    size_t pending_cap = 0;

    int committed_txns = 0;

    off_t journal_start = (off_t)JOURNAL_BLOCK_IDX * (off_t)BLOCK_SIZE;
    off_t offset = journal_start + (off_t)sizeof(struct journal_header);
    off_t journal_end = journal_start + (off_t)jh.nbytes_used;

    while (offset + (off_t)sizeof(struct rec_header) <= journal_end) {
        struct rec_header rh;
        pread_bytes(fd, offset, &rh, sizeof(rh));

        if (rh.size < sizeof(struct rec_header))
            break;
        if (offset + (off_t)rh.size > journal_end)
            break;

        off_t payload = offset + (off_t)sizeof(struct rec_header);

        if (rh.type == REC_DATA) {
            uint16_t expected = (uint16_t)data_record_size();
            if (rh.size != expected)
                break;

            uint32_t block_no;
            uint8_t img[BLOCK_SIZE];

            pread_bytes(fd, payload, &block_no, sizeof(block_no));
            pread_bytes(fd, payload + (off_t)sizeof(block_no), img, BLOCK_SIZE);

            if (block_no >= TOTAL_BLOCKS) {
                fprintf(stderr, "ERROR: Journal DATA targets invalid block %u.\n", block_no);
                free(pending);
                close(fd);
                exit(1);
            }

            if (pending_cnt == pending_cap) {
                size_t new_cap = (pending_cap == 0) ? 16 : pending_cap * 2;
                pending_write_t *tmp = realloc(pending, new_cap * sizeof(*pending));
                if (!tmp)
                    die("realloc pending");
                pending = tmp;
                pending_cap = new_cap;
            }

            pending[pending_cnt].block_no = block_no;
            memcpy(pending[pending_cnt].data, img, BLOCK_SIZE);
            pending_cnt++;

        } else if (rh.type == REC_COMMIT) {
            if (rh.size != (uint16_t)commit_record_size())
                break;

            /* Apply metadata blocks (inode bitmap, inode table) BEFORE directory blocks for consistency */
            for (size_t i = 0; i < pending_cnt; i++) {
                uint32_t blk = pending[i].block_no;
                if (blk == INODE_BMAP_IDX ||
                    (blk >= INODE_START_IDX && blk < INODE_START_IDX + INODE_BLOCKS)) {
                    pwrite_block(fd, blk, pending[i].data);
                }
            }

            /* Then apply directory and other blocks */
            for (size_t i = 0; i < pending_cnt; i++) {
                uint32_t blk = pending[i].block_no;
                if (blk != INODE_BMAP_IDX &&
                    !(blk >= INODE_START_IDX && blk < INODE_START_IDX + INODE_BLOCKS)) {
                    pwrite_block(fd, blk, pending[i].data);
                }
            }

            pending_cnt = 0;
            committed_txns++;

        } else {
            break;
        }

        offset += (off_t)rh.size;
    }

    free(pending);

    /* empty journal means nbytes_used == sizeof(journal_header) */
    uint8_t zero[BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++) {
        pwrite_block(fd, JOURNAL_BLOCK_IDX + i, zero);
    }

    struct journal_header fresh;
    fresh.magic = JOURNAL_MAGIC;
    fresh.nbytes_used = sizeof(struct journal_header);
    journal_write_header(fd, &fresh);

    printf("Installed %d committed transaction(s) from journal.\n", committed_txns);
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s create <name>\n", argv[0]);
        fprintf(stderr, "  %s install\n", argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "create") == 0) {
        if (argc < 3) {
            fprintf(stderr, "ERROR: create requires a filename argument.\n");
            return 1;
        }
        cmd_create(DEFAULT_IMAGE, argv[2]);
        return 0;
    }

    if (strcmp(cmd, "install") == 0) {
        cmd_install(DEFAULT_IMAGE);
        return 0;
    }

    fprintf(stderr, "ERROR: unknown command '%s'\n", cmd);
    return 1;
}

