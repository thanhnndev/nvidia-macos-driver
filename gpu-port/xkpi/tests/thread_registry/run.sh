#!/bin/bash
# run.sh: user-space stress test of the xkpi thread registry (bsdcompat/src/nmx_kernel.c, Step114). The record code is
# extracted verbatim from the source and built against the mocks in stress.c: 16 threads, 24 recycled thread_t values,
# records dropped and reused, once with the real hash and once with every record in one bucket; under TSan and ASan.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
awk '/^#define NMX_THREAD_BUCKETS/{p=1} /^\/\* ---- preemption and CPU/{p=0} p' "$H/../../bsdcompat/src/nmx_kernel.c" |
  sed 's/static struct proc nmx_kernproc_rec = .*/static struct proc nmx_kernproc_rec = { .p_pid = 0 };/' > "$T/region.c"
cp "$H/stress.c" "$T/"
for mode in hash onebucket; do
  [ $mode = onebucket ] && perl -0pi -e 's/(static unsigned bucket_of\(const void \*p, unsigned n\)\n\{\n)/$1    return 0;\n/' "$T/region.c"
  for san in thread address; do
    clang -O1 -fsanitize=$san -w -o "$T/t" "$T/stress.c"
    echo -n "$mode/$san: "; "$T/t"
  done
done
