#!/usr/bin/env python3
"""Put the kernel overlay (xnu/duchamp-overlay.py and xnu/files/) at the end of mac/hakios-kernel.sh.

The Mac script is the one file Isaac downloads to build the kernel; it carries duchamp's additions to the common tree
with it, as a base64 tar.gz after the line __DUCHAMP_OVERLAY__, and applies them after xnu/apply.sh. Run this after
any change under xnu/; it rewrites only what follows the marker.
"""
import base64
import io
import os
import tarfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, 'mac', 'hakios-kernel.sh')
MARKER = '__DUCHAMP_OVERLAY__\n'


def main():
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w:gz', format=tarfile.USTAR_FORMAT) as tar:
        paths = ['xnu/duchamp-overlay.py']
        for root, _, names in os.walk(os.path.join(ROOT, 'xnu', 'files')):
            paths += sorted(os.path.relpath(os.path.join(root, n), ROOT) for n in names)
        for rel in paths:
            info = tar.gettarinfo(os.path.join(ROOT, rel), arcname=rel)
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            info.mtime = 0
            with open(os.path.join(ROOT, rel), 'rb') as f:
                tar.addfile(info, f)
    blob = base64.encodebytes(buf.getvalue()).decode()
    with open(SCRIPT) as f:
        s = f.read()
    at = s.find(MARKER)
    if at < 0:
        raise SystemExit(f'embed-overlay: no line {MARKER.strip()} in {SCRIPT}')
    s = s[:at + len(MARKER)] + blob
    with open(SCRIPT, 'w') as f:
        f.write(s)
    print(f'embed-overlay: {len(paths)} files, {len(blob)} bytes of base64 in mac/hakios-kernel.sh')


if __name__ == '__main__':
    main()
