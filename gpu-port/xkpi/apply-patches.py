#!/usr/bin/env python3
"""Replay the ordered patch set from the vendor Git HEAD before changing files.
Accept only pristine, known intermediate, or final contents. Refuse unknown local
edits. Never reapply old hunks to a tree already changed by their successors.
"""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
vendor = Path(os.environ.get('GPU_SRC', str(Path.home()/'develop/projects/gpu-src')))/'drm-kmod'
patchdir = here/'patches'
names = [s.strip() for s in (patchdir/'series').read_text().splitlines() if s.strip() and not s.startswith('#')]
if len(names) != len(set(names)) or set(names) != {p.name for p in patchdir.glob('*.patch')}:
    raise SystemExit('patch series must name each patch exactly once')
paths = set()
for name in names:
    for line in (patchdir/name).read_text().splitlines():
        if line.startswith('+++ b/'):
            relative = line[6:].split('\t')[0]
            if Path(relative).is_absolute() or '..' in Path(relative).parts:
                raise SystemExit('invalid patch path')
            paths.add(relative)
known = {p: set() for p in paths}
def digest(data): return hashlib.sha256(data).hexdigest()
with tempfile.TemporaryDirectory(prefix='nmx-patches-') as temp:
    stage = Path(temp)
    for p in paths:
        data = subprocess.check_output(['git', '-C', str(vendor), 'show', 'HEAD:'+p])
        f = stage/p; f.parent.mkdir(parents=True, exist_ok=True); f.write_bytes(data)
        known[p].add(digest(data))
    for name in names:
        r = subprocess.run(['patch', '--batch', '--forward', '--fuzz=0', '-p1', '-d', str(stage)],
                           input=(patchdir/name).read_bytes(), capture_output=True)
        if r.returncode:
            raise SystemExit(f'patch replay failed: {name}\n{r.stdout.decode()}\n{r.stderr.decode()}')
        for p in paths:
            known[p].add(digest((stage/p).read_bytes()))
    # Validate the complete tree before touching any source.
    for p in paths:
        if digest((vendor/p).read_bytes()) not in known[p]:
            raise SystemExit(f'unknown local changes in {vendor/p}; preserve/reconcile them first')
    changed = 0
    for p in sorted(paths):
        data = (stage/p).read_bytes()
        if (vendor/p).read_bytes() != data:
            (vendor/p).write_bytes(data); changed += 1
    print(f'patch replay: {len(names)} patches, {len(paths)} files, {changed} updated')
