# VSFS — A Journaling File System in C

[![CI](https://github.com/UtshaBasak/CSE321_VSFS/actions/workflows/ci.yml/badge.svg)](https://github.com/UtshaBasak/CSE321_VSFS/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Language: C](https://img.shields.io/badge/C-C11-00599C.svg)](include/vsfs.h)

A minimal Unix-style file system implemented from scratch on a flat disk
image, with a write-ahead journal that makes file creation crash-atomic and a
consistency checker that proves the result is sound.

Built for **CSE321 (Operating Systems)**.

The project is three small, self-contained C programs sharing one on-disk
format definition:

| Tool        | What it does                                                          |
| ----------- | --------------------------------------------------------------------- |
| `mkfs`      | Formats a fresh 348 KB image: superblock, bitmaps, inode table, root directory |
| `journal`   | Stages file creation in a write-ahead log, then replays committed transactions |
| `validator` | An `fsck` for the image: checks bitmaps, inodes, directories and link counts |

---

## Why a journal

Creating a single file touches three separate blocks — the inode bitmap, the
inode table and the root directory. Write them one at a time and a crash
between any two leaves the filesystem inconsistent: an allocated inode nothing
points to, or worse, a directory entry pointing at an inode that was never
allocated.

VSFS solves this the way ext3/ext4 do. `journal create` writes *nothing* to
the filesystem itself. It appends the three updated blocks to a log, then
seals them with a commit record. `journal install` replays the log, but only
for transactions that reached a commit — anything after the last commit is
discarded as a half-finished write.

The result is atomicity: after a crash the file either exists completely or
does not exist at all. There is no state in between.

```
journal create notes.txt          journal install
┌──────────────────────┐          ┌──────────────────────┐
│ inode bitmap  → DATA │          │  commit found?       │
│ inode table   → DATA │  ──────► │  yes → write blocks  │
│ root dir      → DATA │          │  no  → discard       │
│ COMMIT               │          └──────────────────────┘
└──────────────────────┘
      journal only               filesystem finally updated
```

---

## Disk layout

85 blocks of 4096 bytes each, 348,160 bytes total.

```
 block   0        1 ............ 16   17     18     19 .. 20   21 ........ 84
       ┌─────┬──────────────────────┬──────┬──────┬──────────┬──────────────┐
       │ SB  │       journal        │ ibmp │ dbmp │  inodes  │     data     │
       └─────┴──────────────────────┴──────┴──────┴──────────┴──────────────┘
         1            16 blocks        1      1      2 blocks     64 blocks
                                                    (64 inodes)
```

- **Inodes** are 128 bytes with 8 direct pointers and no indirect blocks, so a
  file tops out at 32 KiB.
- **Directory entries** are 32 bytes (4-byte inode number + 28-byte name), so
  the single root directory block holds 128 entries.
- **Inode 0** is always the root directory, which is why a directory slot is
  free only when its inode number is 0 *and* its name is empty.

The full format, the journal record layout and the crash-recovery reasoning
are written up in [docs/DESIGN.md](docs/DESIGN.md).

---

## Build

Requires a C11 compiler and GNU make on a POSIX system (Linux, macOS, or WSL
on Windows).

```sh
make
```

Binaries land in `bin/`. Other targets:

```sh
make image      # format a fresh vsfs.img
make check      # build and run the test suite
make debug      # rebuild with AddressSanitizer and UBSan
make clean      # remove build output
make help       # list targets
```

---

## Usage

```sh
mkfs [image]          # format an image          (default: vsfs.img)
journal create NAME   # stage a new file in the journal
journal install       # replay committed transactions onto the filesystem
validator [image]     # check consistency; exits 0 if clean, 1 if not
```

`journal` always works on `./vsfs.img` in the current directory.

### A session

```console
$ make
$ ./bin/mkfs vsfs.img
Created VSFS image 'vsfs.img' (85 blocks).

$ ./bin/validator vsfs.img
Filesystem 'vsfs.img' is consistent.

$ ./bin/journal create notes.txt
File 'notes.txt' created (inode 1).

$ ./bin/journal create todo.md
File 'todo.md' created (inode 2).
```

At this point the filesystem is untouched — both creations are sitting in the
journal, committed but not installed. The image is still perfectly consistent,
which is the whole point:

```console
$ ./bin/validator vsfs.img
Filesystem 'vsfs.img' is consistent.

$ ./bin/journal install
Installed 2 committed transaction(s) from journal.

$ ./bin/validator vsfs.img
Filesystem 'vsfs.img' is consistent.
```

### Watching the validator catch corruption

```console
$ printf '\xff\xff\xff\xff' | dd of=vsfs.img bs=1 seek=0 conv=notrunc
$ ./bin/validator vsfs.img
ERROR: invalid superblock magic 0xffffffff
1 inconsistencies found.
$ echo $?
1
```

---

## Testing

```sh
make check
```

The suite in [tests/run_tests.sh](tests/run_tests.sh) runs each case in its
own scratch directory and covers formatting, image geometry, the fact that
`create` leaves the filesystem untouched until `install`, batched
transactions, rejection of duplicate and over-long names, and validator
detection of a corrupt superblock and an orphaned bitmap bit.

Every push and pull request runs the same suite on GitHub Actions against both
GCC and Clang, plus a third job built with AddressSanitizer and UBSan. The
workflow is [.github/workflows/ci.yml](.github/workflows/ci.yml).

---

## Project structure

```
.
├── include/
│   └── vsfs.h            on-disk format: the single source of truth
├── src/
│   ├── mkfs.c            image formatter
│   ├── journal.c         write-ahead log: create and install
│   └── validator.c       consistency checker
├── tests/
│   └── run_tests.sh      end-to-end test suite
├── docs/
│   └── DESIGN.md         format spec and crash-recovery analysis
├── .github/
│   ├── workflows/ci.yml  build and test on GCC, Clang and sanitizers
│   └── dependabot.yml    keeps the pinned action versions current
├── Makefile
└── README.md
```

Every structure size in `vsfs.h` is pinned by a `_Static_assert`, so a change
that would silently shift the on-disk layout fails to compile instead.

---

## Limitations

Deliberate, and scoped to the assignment:

- One directory (the root) — no `mkdir`, no path resolution.
- `create` makes empty files; there is no read, write or delete path.
- Fixed geometry, set at compile time.
- Host byte order, so images are not portable across endianness.
- The journal is not checksummed, and nothing calls `fsync()` — ordering is
  guaranteed against process crashes, not against power loss with a volatile
  disk cache.

---

## Author

**Utsha Basak** — [@UtshaBasak](https://github.com/UtshaBasak)

## License

Released under the [MIT License](LICENSE).
