#!/usr/bin/env python3
"""Apply scripts/core-patches/*.patch to the patched core, in name order.

The round scripts write files with Path.write_text, which gives CRLF on Windows,
so every file a patch touches is normalised to LF first; then git apply.
"""
import pathlib
import re
import subprocess
import sys

source = pathlib.Path(sys.argv[1]).resolve()
patch_dir = pathlib.Path(__file__).resolve().parent / 'core-patches'

for patch in sorted(patch_dir.glob('*.patch')):
    text = patch.read_bytes().replace(b'\r\n', b'\n')
    for rel in sorted(set(re.findall(rb'^diff --git a/(\S+) b/', text, re.M))):
        target = source / rel.decode()
        if target.exists():
            data = target.read_bytes()
            if b'\r\n' in data:
                target.write_bytes(data.replace(b'\r\n', b'\n'))
    normalised = patch_dir.parent.parent / 'build' / ('lf-' + patch.name)
    normalised.parent.mkdir(parents=True, exist_ok=True)
    normalised.write_bytes(text)
    for check in (True, False):
        args = ['git', '-C', str(source), 'apply', '-C1', '--whitespace=nowarn']
        if check:
            args.append('--check')
        result = subprocess.run(args + [str(normalised)], capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout + result.stderr)
            sys.exit(f'Core patch {patch.name} did not apply')
    print(f'Applied core patch {patch.name}')
