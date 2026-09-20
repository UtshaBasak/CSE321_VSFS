# VSFS Design Notes

This document describes the on-disk format and the journaling protocol that
the three tools in this repository implement. The authoritative definition of
every constant and structure is [`include/vsfs.h`](../include/vsfs.h); this
document explains the reasoning behind them.

## 1. Disk layout

A VSFS image is exactly 85 blocks of 4096 bytes (348,160 bytes). The geometry
is fixed at compile time - there is no resizing.

| Blocks  | Count | Region       | Contents                                        |
| ------- | ----- | ------------ | ----------------------------------------------- |
| 0       | 1     | Superblock   | Magic number and the offset of every region      |
| 1-16    | 16    | Journal      | Write-ahead log, 64 KiB                          |
| 17      | 1     | Inode bitmap | One bit per inode, bit *i* set = inode *i* used  |
| 18      | 1     | Data bitmap  | One bit per data block, relative to block 21     |
| 19-20   | 2     | Inode table  | 64 inodes of 128 bytes                           |
| 21-84   | 64    | Data         | File and directory contents                      |

Both bitmaps occupy a full block but use only their first bits: 64 bits for
inodes and 64 for data blocks. The validator treats any bit set beyond the
valid range as corruption.

## 2. Structures

All three structures are `__attribute__((packed))` and their sizes are
enforced by `_Static_assert`, so a layout mistake is a compile error rather
than a silent corruption.

### Superblock (128 bytes, block 0)

Nine `uint32_t` fields followed by padding: `magic` (`0x56534653`, "VSFS"),
`block_size`, `total_blocks`, `inode_count`, and the starting block of the
journal, inode bitmap, data bitmap, inode table and data region. Storing the
offsets rather than deriving them means the validator can check the image
against the geometry the tools were built with.

### Inode (128 bytes, 32 per block)

| Field       | Type          | Meaning                                       |
| ----------- | ------------- | --------------------------------------------- |
| `type`      | `uint16_t`    | 0 free, 1 regular file, 2 directory            |
| `links`     | `uint16_t`    | Directory entries that refer to this inode     |
| `size`      | `uint32_t`    | Size in bytes                                  |
| `direct[8]` | `uint32_t[8]` | Absolute block numbers; 0 means unused         |
| `ctime`     | `uint32_t`    | Creation time, seconds since the epoch         |
| `mtime`     | `uint32_t`    | Last modification time                         |

Eight direct pointers and no indirect blocks cap a file at 32 KiB, which is
half the data region - a deliberate simplification.

Inode 0 is always the root directory. `mkfs` creates it with `links = 2`,
because both `.` and `..` in the root refer to it.

### Directory entry (32 bytes, 128 per block)

A `uint32_t` inode number followed by a 28-byte NUL-terminated name. A slot is
free when the inode number is 0 *and* the name is empty, which is why inode 0
can never be handed out to a regular file. One block holds 128 entries, and
that is the hard limit on the number of files in the root directory.

## 3. Journaling

The journal is a linear, append-only log in blocks 1-16. Its first eight bytes
are a header:

    struct journal_header { uint32_t magic; uint32_t nbytes_used; };

`nbytes_used` is the offset at which the next record is written, and it starts
at 8 (the size of the header itself) for an empty journal. Records follow the
header back to back, each starting with a 4-byte `{ type, size }` header:

| Record   | Type     | Size       | Payload                                     |
| -------- | -------- | ---------- | ------------------------------------------- |
| `DATA`   | `0x0001` | 4104 bytes | Target block number (4 bytes) + a full 4096-byte block image |
| `COMMIT` | `0x0002` | 4 bytes    | None                                        |

Whole block images are logged rather than byte-level deltas. It costs space,
but replay becomes an idempotent `pwrite` of each image: no merging, and
replaying the same journal twice produces the same result.

### Write path: `journal create`

`create` never writes to the filesystem regions. It:

1. Reads the inode bitmap, the inode table and the root directory block.
2. Replays any **committed** transactions already in the journal on top of
   that in-memory copy, so two `create` calls in a row see each other's work
   without an intervening `install`.
3. Allocates the lowest free inode and the lowest free directory slot, and
   builds the three updated blocks in memory.
4. Appends them as three `DATA` records, then a `COMMIT` record.
5. Rewrites the eight-byte journal header last.

The header write is the linearisation point. Until `nbytes_used` grows, the
appended records sit outside the journal's valid range and are invisible to
both `install` and the next `create`.

Before appending, `create` checks that the whole transaction - three data
records plus a commit, 12,316 bytes - fits in the space left. The 64 KiB
journal therefore holds five staged creations; the sixth fails with a message
telling you to run `install`.

### Replay path: `journal install`

`install` walks the journal from the header to `nbytes_used`, buffering `DATA`
records. On a `COMMIT` it flushes the buffer to the home blocks and clears it.
Records that follow the last `COMMIT` are never flushed - they are what a
crash mid-transaction leaves behind, and discarding them is exactly what makes
the write path atomic. Parsing also stops early on any malformed record, so a
torn write truncates the log rather than corrupting the filesystem.

Within a transaction the flush is ordered: the inode bitmap and inode table
first, the directory block second. If the machine dies between the two, the
image holds an allocated but unreferenced inode - wasted space that the
validator reports, rather than a directory entry pointing at a free inode,
which would be a dangling reference.

After replay the 16 journal blocks are zeroed and the header is rewritten
empty.

### Crash matrix

| Crash point                    | State after reboot              | `install` does |
| ------------------------------ | ------------------------------- | -------------- |
| During `create`, before header | Records outside the valid range | Nothing        |
| After header, before `install` | Committed transaction on disk   | Replays it     |
| Mid-`install`, before flush    | Journal still intact            | Replays again  |
| Mid-`install`, mid-flush       | Some blocks written             | Rewrites all   |

## 4. Validation

`validator` opens the image read-only and reports every problem it finds
rather than stopping at the first:

- **Superblock** - magic, block size, total blocks, inode count and every
  region offset must match the compiled-in geometry.
- **Allocation agreement** - an inode's `type` field and its bit in the inode
  bitmap must agree in both directions.
- **Pointer range** - every non-zero `direct[]` entry must land inside the
  data region.
- **Duplicate blocks** - no data block may be claimed by two inodes.
- **Size against pointers** - an inode must reference at least
  `ceil(size / 4096)` blocks, and a zero-size inode must reference none.
- **Directory structure** - size must be a multiple of 32; `.` must point at
  the directory itself; `..` must be present; names must be NUL-terminated and
  non-empty; every referenced inode must be in range and allocated.
- **Link counts** - an inode's `links` must equal the number of directory
  entries that actually refer to it.
- **Bitmap agreement** - a set data-bitmap bit must correspond to a block some
  inode references, and the reverse; bits beyond the valid range must be clear.

The exit status is 0 when the image is consistent and 1 when it is not, so the
validator drops straight into a shell pipeline or a test script.

## 5. Known limitations

These follow from the scope of the assignment rather than being oversights:

- The root directory is the only directory; there is no `mkdir` and no path
  resolution.
- `create` makes empty files. There is no write, read or delete path, so no
  data blocks are allocated after `mkfs`.
- `journal` always operates on `./vsfs.img`; only `mkfs` and `validator` take
  an image path.
- Multi-byte fields use host byte order, so an image is not portable between a
  little-endian and a big-endian machine.
- The journal is not checksummed. A record whose header survives a torn write
  but whose payload does not would be replayed as-is.
- Nothing calls `fsync()`, so ordering holds against process crashes but not
  against power loss with a volatile disk cache.
