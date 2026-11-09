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

def apply(patch, required):
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
            if required:
                print(result.stdout + result.stderr)
                sys.exit(f'Core patch {patch.name} did not apply')
            print(f'Skipped optional core patch {patch.name}: does not apply to this source')
            return False
    print(f'Applied core patch {patch.name}')
    return True

for patch in sorted(patch_dir.glob('*.patch')):
    apply(patch, True)

# Optional fixes ported from Vita3K-Plus: each is its own patch and one that
# does not apply is skipped, so a source that moved on never blocks a build.
applied_optional = 0
for patch in sorted((patch_dir / 'optional').glob('*.patch')):
    if apply(patch, False):
        applied_optional += 1
if applied_optional:
    # translated shaders change, so cached ones from an earlier build are redone
    header = source / 'vita3k/shader/include/shader/spirv_recompiler.h'
    text = header.read_text()
    match = re.search(r'(CURRENT_VERSION = )(\d+)', text)
    if match:
        header.write_text(text[:match.start(2)] + str(int(match.group(2)) + 1) + text[match.end(2):])
        print(f'Shader cache version raised to {int(match.group(2)) + 1}')
