#!/bin/sh
# CPU-only test of the descriptor snapshot path + desc-cache (see desccache_test.c). No GPU, no driver.
set -e
D=$(cd "$(dirname "$0")" && pwd)
VKH=${VKH:-$D/../../out/vk/include}
[ -d "$VKH/vulkan" ] || VKH=$HOME/develop/projects/nmx-plugin-890m/out/vk/include
O=${TMPDIR:-/tmp}/desccache-test.$$; mkdir -p "$O"
python3 "$D/desccache_extract.py" "$D/../nvmtl_vk.c" > "$O/desccache_extracted.inc"
cp "$D/../nvmtl_desc_cache.h" "$O/"
xcrun clang -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wno-unused-function -Wno-unused-variable \
    -I"$VKH" -I"$O" -I"$D/.." -o "$O/desccache_test" "$D/desccache_test.c"
"$O/desccache_test" "$@"
rc=$?; rm -rf "$O"; exit $rc
