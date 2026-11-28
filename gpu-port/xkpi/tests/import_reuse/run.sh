#!/bin/bash
# Userspace test of the import reuse cut (nmx_page.c: idle BK_USER records, pin entry blocks). Extracts the registry,
# page record and user pin code from ../../bsdcompat/src/nmx_page.c as it is, compiles it with stubs, runs the checks.
#   bash tests/import_reuse/run.sh        (from xkpi/, or anywhere)
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../bsdcompat/src/nmx_page.c
out=${TMPDIR:-/tmp}/import_reuse_test.$$
mkdir -p "$out"
awk '/^#define REG_BUCKETS/{f=1} /^static void backing_release\(struct backing \*bk\);/{f=0} f' "$src" > "$out/s1.inc"
awk '/^static void page_record_init/{f=1} f{print} f&&/^}/{exit}' "$src" > "$out/s2.inc"
awk '/^struct user_pin \{/{f=1} f{print} f&&/^void nmx_upin_destroy/{g=1} g&&/^}/{exit}' "$src" > "$out/s3.inc"
awk '/^int nmx_page_init\(void\)/{f=1} f{print} f&&/^}/{exit}' "$src" > "$out/s5.inc"
awk '/^vm_page_t nmx_phys_to_page/{f=1} f{print} f&&/^}/{exit}' "$src" > "$out/s4.inc"
xcrun clang -std=gnu11 -g -O1 -fsanitize=address,undefined -Wall -Wno-unused-function -Wno-unused-variable \
    -I"$here" -I"$out" -o "$out/t" "$here/import_reuse_test.c"
"$out/t"
rm -rf "$out"
