"""Validate manifest, assets, project references and pinned upstream metadata."""
# SPDX-License-Identifier: GPL-2.0-or-later
import json
from pathlib import Path
import re
import struct
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
app = root / 'src/Vita3K.UWP'
ns = {'m': 'http://schemas.microsoft.com/appx/manifest/foundation/windows10',
      'u': 'http://schemas.microsoft.com/appx/manifest/uap/windows10',
      'b': 'http://schemas.microsoft.com/developer/msbuild/2003'}
manifest = ET.parse(app / 'Package.appxmanifest')
identity = manifest.find('m:Identity', ns)
assert identity.attrib['ProcessorArchitecture'] == 'x64'
assert identity.attrib['Publisher'] == 'CN=Vita3K-UWP'
assert re.fullmatch(r'\d+\.\d+\.\d+\.\d+', identity.attrib['Version'])
assert all(0 <= int(n) <= 65535 for n in identity.attrib['Version'].split('.'))
application = manifest.find('m:Applications/m:Application', ns)
assert application.attrib['EntryPoint'] == 'Vita3KUWP.App'
assert 'runFullTrust' not in (app / 'Package.appxmanifest').read_text()
for name, expected in [('StoreLogo',(50,50)),('Square44x44Logo',(44,44)),('Square150x150Logo',(150,150)),('SplashScreen',(620,300))]:
    data = (app / 'Assets' / (name + '.png')).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    assert struct.unpack('>II', data[16:24]) == expected, name
project = ET.parse(app / 'Vita3K.UWP.vcxproj')
for item in project.findall('.//b:ClCompile', ns) + project.findall('.//b:AppxManifest', ns):
    if 'Include' in item.attrib:
        assert (app / item.attrib['Include'].replace('\\', '/')).is_file()
lock = json.loads((root / 'upstream.lock.json').read_text())
assert lock['repository'] == 'https://github.com/Vita3K/Vita3K.git'
assert re.fullmatch(r'[0-9a-f]{40}', lock['commit'])
for doc in [root / 'README.md', *root.glob('docs/*.md'), root / 'CONTRIBUTING.md']:
    for target in re.findall(r'\]\(([^)]+)\)', doc.read_text()):
        if '://' not in target and not target.startswith('#'):
            assert (doc.parent / target.split('#')[0]).exists(), (doc, target)
assert 'GNU GENERAL PUBLIC LICENSE' in (root / 'COPYING.txt').read_text()
print('Manifest, project, PNG assets, documentation links and upstream lock validated.')
