#!/usr/bin/env python3
"""Fresh/repeated ordered replay, conflict preservation, stale-object rejection.
Uses the existing vendor checkout's HEAD as fixture input, with no GPU access.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

here = Path(__file__).resolve().parents[1]
vendor = Path(os.environ.get('GPU_SRC', str(Path.home()/'develop/projects/gpu-src')))/'drm-kmod'
paths = set()
for patch in (here/'patches').glob('*.patch'):
    for line in patch.read_text().splitlines():
        if line.startswith('+++ b/'):
            paths.add(line[6:].split('\t')[0])
with tempfile.TemporaryDirectory(prefix='nmx-replay-test-') as temp:
    root = Path(temp); checkout = root/'drm-kmod'; checkout.mkdir()
    for path in paths:
        f = checkout/path; f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(subprocess.check_output(['git','-C',str(vendor),'show','HEAD:'+path]))
    def git(*args):
        subprocess.run(['git','-C',str(checkout),*args],check=True,capture_output=True)
    git('init','-q'); git('add','.'); git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','-qm','vendor fixture')
    env = dict(os.environ, GPU_SRC=str(root))
    def replay(): return subprocess.run(['python3', str(here/'apply-patches.py')],env=env,capture_output=True,text=True)
    r = replay(); assert r.returncode == 0, r.stderr
    first = {p:(checkout/p).read_bytes() for p in paths}
    r = replay(); assert r.returncode == 0 and '0 updated' in r.stdout, (r.stdout,r.stderr)
    assert all((checkout/p).read_bytes()==v for p,v in first.items())
    path = sorted(paths)[0]; (checkout/path).write_bytes(first[path]+b'\n/* owner edit */\n')
    before = {p:(checkout/p).read_bytes() for p in paths}
    r = replay(); assert r.returncode != 0 and 'unknown local changes' in r.stderr
    assert all((checkout/p).read_bytes()==v for p,v in before.items())
    # Force one.sh's compiler to fail and ensure the old object cannot survive.
    build = root/'build'; build.mkdir(); (build/'logs').mkdir(); (build/'obj').mkdir()
    shutil.copy(here/'one.sh',build/'one.sh')
    (build/'cc.sh').write_text('#!/bin/bash\nexit 1\n'); (build/'cc.sh').chmod(0o755)
    (build/'obj/fake.o').write_bytes(b'stale')
    r = subprocess.run(['bash','one.sh','fake.c'],cwd=build,capture_output=True)
    assert r.returncode != 0 and not (build/'obj/fake.o').exists() and (build/'logs/fake.failed').exists()
    # Direct linking refuses an outstanding compile failure before any compiler call.
    (build/'kext').mkdir(); shutil.copy(here/'kext/build.sh',build/'kext/build.sh')
    # build.sh expects GPU_SRC/xkpi; point it at our fixture.
    (root/'xkpi').symlink_to(build)
    r = subprocess.run(['bash',str(build/'kext/build.sh'),str(root/'unused')],env=dict(os.environ,GPU_SRC=str(root)),capture_output=True)
    assert r.returncode != 0 and b'refusing stale-object link' in r.stdout
    (build/'logs/fake.failed').unlink()
    (build/'.nmx-driver-build-incomplete').touch()
    r = subprocess.run(['bash',str(build/'kext/build.sh'),str(root/'unused')],env=dict(os.environ,GPU_SRC=str(root)),capture_output=True)
    assert r.returncode != 0 and b'.nmx-driver-build-incomplete' in r.stdout
print('PASS: clean replay, repeat identity, unknown-edit preservation, failed-object removal, link refusal')
