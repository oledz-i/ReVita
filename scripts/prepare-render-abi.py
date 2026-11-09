"""Fetch exact public ABI headers; no executable binaries are downloaded."""
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import hashlib,subprocess,urllib.request,json
root=Path(__file__).resolve().parents[1]
out=root/'build/abi';out.mkdir(parents=True,exist_ok=True)
vulkan=out/'Vulkan-Headers'
if not (vulkan/'.git').exists():
    subprocess.run(['git','clone','--depth','1','--branch','v1.3.290','https://github.com/KhronosGroup/Vulkan-Headers.git',str(vulkan)],check=True)
actual=subprocess.check_output(['git','-C',str(vulkan),'rev-parse','HEAD'],text=True).strip()
if actual!='b379292b2ab6df5771ba9870d53cf8b2c9295daf':raise RuntimeError('Vulkan header revision mismatch')
lock=json.loads((root/'render-abi.lock.json').read_text())
include=out/'include';include.mkdir(exist_ok=True)
for name,digest in lock['headers'].items():
    data=urllib.request.urlopen('https://raw.githubusercontent.com/WizzardSK/vita3k-libretro/'+lock['core_commit']+'/vita3k/libretro/'+name,timeout=60).read()
    if hashlib.sha256(data).hexdigest()!=digest:raise RuntimeError('Header digest mismatch: '+name)
    (include/name).write_bytes(data)
