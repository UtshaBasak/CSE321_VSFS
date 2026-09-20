/*
 * mkfs.c - Format a VSFS disk image.
 *
 * Writes a complete, empty filesystem: superblock, zeroed journal region,
 * both bitmaps, an inode table containing only the root directory, and a
 * data region whose first block holds the root directory's "." and ".."
 * entries.
 *
 * Usage: mkfs [image]      (default: vsfs.img)
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vsfs.h"

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void write_block(int fd, const void *block) {
    ssize_t written = write(fd, block, BLOCK_SIZE);
    if (written != (ssize_t)BLOCK_SIZE) {
        die("write");
    }
}

static void set_bitmap(uint8_t *bitmap, uint32_t index) {
    bitmap[index / 8] |= (uint8_t)(1U << (index % 8));
}

int main(int argc, char *argv[]) {
    const char *image_path = (argc > 1) ? argv[1] : DEFAULT_IMAGE;

    int fd = open(image_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        die("open");
    }

    uint8_t block[BLOCK_SIZE];
    memset(block, 0, sizeof(block));

    struct superblock sb = {
        .magic = FS_MAGIC,
        .block_size = BLOCK_SIZE,
        .total_blocks = TOTAL_BLOCKS,
        .inode_count = INODE_COUNT,
        .journal_block = JOURNAL_BLOCK_IDX,
        .inode_bitmap = INODE_BMAP_IDX,
        .data_bitmap = DATA_BMAP_IDX,
        .inode_start = INODE_START_IDX,
        .data_start = DATA_START_IDX,
    };

    memcpy(block, &sb, sizeof(sb));
    write_block(fd, block); // Superblock

    memset(block, 0, sizeof(block));
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; ++i) {
        write_block(fd, block); // Journal blocks
    }

    memset(block, 0, sizeof(block));
    set_bitmap(block, 0); // Reserve inode 0 for root
    write_block(fd, block); // Inode bitmap

    memset(block, 0, sizeof(block));
    set_bitmap(block, 0); // Reserve first data block for root directory
    write_block(fd, block); // Data bitmap

    time_t now = time(NULL);

    struct inode root = {0};
    root.type = INODE_DIR;
    root.links = 2; /* "." and ".." both point at the root */
    root.size = 2 * sizeof(struct dirent);
    memset(root.direct, 0, sizeof(root.direct));
    root.direct[0] = DATA_START_IDX;
    root.ctime = (uint32_t)now;
    root.mtime = (uint32_t)now;

    memset(block, 0, sizeof(block));
    memcpy(block, &root, sizeof(root));
    write_block(fd, block); // First inode block

    memset(block, 0, sizeof(block));
    write_block(fd, block); // Second inode block

    memset(block, 0, sizeof(block));
    struct dirent *root_dirents = (struct dirent *)block;
    root_dirents[0].inode = 0;
    strncpy(root_dirents[0].name, ".", sizeof(root_dirents[0].name) - 1);
    root_dirents[0].name[sizeof(root_dirents[0].name) - 1] = '\0';
    root_dirents[1].inode = 0;
    strncpy(root_dirents[1].name, "..", sizeof(root_dirents[1].name) - 1);
    root_dirents[1].name[sizeof(root_dirents[1].name) - 1] = '\0';
    write_block(fd, block); // First data block holds root directory entries

    memset(block, 0, sizeof(block));
    for (uint32_t i = 1; i < DATA_BLOCKS; ++i) {
        write_block(fd, block);
    }

    if (close(fd) < 0) {
        die("close");
    }

    printf("Created VSFS image '%s' (%u blocks).\n", image_path, TOTAL_BLOCKS);
    return 0;
}

