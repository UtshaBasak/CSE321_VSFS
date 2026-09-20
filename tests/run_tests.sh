#!/usr/bin/env bash
#
# Smoke tests for the VSFS tools.
#
# Run from the repository root after `make`, or simply `make check`.
# Every test runs in its own scratch directory, because `journal` always
# operates on ./vsfs.img in the current working directory.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/bin"
MKFS="$BIN/mkfs"
VALIDATOR="$BIN/validator"
JOURNAL="$BIN/journal"

PASS=0
FAIL=0

for tool in "$MKFS" "$VALIDATOR" "$JOURNAL"; do
    if [ ! -x "$tool" ]; then
        echo "error: $tool not found. Run 'make' first." >&2
        exit 2
    fi
done

ok()   { PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m  %s\n' "$1"; }

# check <description> <expected-exit-status> <command...>
check() {
    local desc="$1" want="$2"; shift 2
    local out status
    out="$("$@" 2>&1)"; status=$?
    if [ "$status" -eq "$want" ]; then
        ok "$desc"
    else
        bad "$desc (exit $status, wanted $want)"
        printf '        %s\n' "$out"
    fi
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 2

echo "VSFS test suite"
echo

# --- formatting -------------------------------------------------------
check "mkfs formats a fresh image"            0 "$MKFS" vsfs.img

SIZE=$(wc -c < vsfs.img)
if [ "$SIZE" -eq $((85 * 4096)) ]; then
    ok "image is 85 blocks (348160 bytes)"
else
    bad "image is $SIZE bytes, expected $((85 * 4096))"
fi

check "fresh image passes the validator"      0 "$VALIDATOR" vsfs.img

# --- staging a transaction -------------------------------------------
check "create stages a new file"              0 "$JOURNAL" create notes.txt

# create must not touch the filesystem itself, only the journal
if dd if=vsfs.img bs=4096 skip=21 count=1 2>/dev/null | grep -aq "notes.txt"; then
    bad "create wrote into the root directory before install"
else
    ok "create leaves the root directory untouched"
fi

check "image is still consistent before install" 0 "$VALIDATOR" vsfs.img

# --- installing -------------------------------------------------------
check "install replays the committed transaction" 0 "$JOURNAL" install

if dd if=vsfs.img bs=4096 skip=21 count=1 2>/dev/null | grep -aq "notes.txt"; then
    ok "install wrote the entry into the root directory"
else
    bad "install did not write the entry into the root directory"
fi

check "image is consistent after install"     0 "$VALIDATOR" vsfs.img
check "install on an empty journal is a no-op" 0 "$JOURNAL" install

# --- error handling ---------------------------------------------------
check "duplicate filename is rejected"        1 "$JOURNAL" create notes.txt
check "over-long filename is rejected"        1 "$JOURNAL" create \
    "this-name-is-far-longer-than-twenty-seven-characters"
check "unknown command is rejected"           1 "$JOURNAL" frobnicate
check "create without an argument is rejected" 1 "$JOURNAL" create

# --- several transactions in one journal ------------------------------
"$MKFS" vsfs.img > /dev/null
for name in alpha beta gamma delta; do
    "$JOURNAL" create "$name" > /dev/null
done
check "four staged transactions install together" 0 "$JOURNAL" install

FOUND=0
for name in alpha beta gamma delta; do
    if dd if=vsfs.img bs=4096 skip=21 count=1 2>/dev/null | grep -aq "$name"; then
        FOUND=$((FOUND + 1))
    fi
done
if [ "$FOUND" -eq 4 ]; then
    ok "all four entries are present in the root directory"
else
    bad "only $FOUND of 4 entries present in the root directory"
fi

check "image is consistent after a batch install" 0 "$VALIDATOR" vsfs.img

# --- corruption is detected -------------------------------------------
"$MKFS" vsfs.img > /dev/null
printf '\xff\xff\xff\xff' | dd of=vsfs.img bs=1 seek=0 conv=notrunc 2>/dev/null
check "validator rejects a bad superblock magic" 1 "$VALIDATOR" vsfs.img

"$MKFS" vsfs.img > /dev/null
# set a stray bit in the data bitmap for a block no inode references
printf '\x03' | dd of=vsfs.img bs=1 seek=$((18 * 4096)) conv=notrunc 2>/dev/null
check "validator detects an orphaned data bitmap bit" 1 "$VALIDATOR" vsfs.img

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
