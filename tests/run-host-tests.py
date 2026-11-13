#!/usr/bin/env python3
"""Compile the real client with a host IOKit model; exercise overlay preflight/idempotence."""
import base64, hashlib, importlib.util, io, pathlib, subprocess, tarfile, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('overlay', ROOT/'xnu/duchamp-overlay.py')
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)
def digest(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}
with tempfile.TemporaryDirectory() as tmp:
    tmp=pathlib.Path(tmp)
    for h in ['IOLib.h','IOLocks.h','IOKitKeys.h','IOPlatformExpert.h','IOService.h','IOUserClient.h','IOBufferMemoryDescriptor.h']:
        p=tmp/'IOKit'/h; p.parent.mkdir(exist_ok=True); p.write_text('#include "iokit_mock.hpp"\n')
    for h in ['mach/vm_param.h','sys/systm.h']:
        p=tmp/h; p.parent.mkdir(exist_ok=True); p.write_text('#include "iokit_mock.hpp"\n')
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-pthread','-g',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-DKERNEL',
                    '-I'+str(tmp),'-I'+str(ROOT/'tests'),'-I'+str(ROOT/'xnu/files/bsd'),
                    str(ROOT/'tests/panthor_host.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
    tree=tmp/'tree'; tree.mkdir()
    # Build a minimal anchor fixture, excluding anchors inserted by prior edits.
    prior=''
    for rel,anchor,text,_ in overlay.EDITS:
        if anchor not in prior:
            p=tree/rel; p.parent.mkdir(parents=True,exist_ok=True)
            with p.open('a') as f: f.write(anchor+'\n')
        prior+=text
    (tree/'bsd/dev').mkdir(parents=True,exist_ok=True)
    (tree/'bsd/dev/redwood_emmc.c').touch()
    cmd=['python3',str(ROOT/'xnu/duchamp-overlay.py'),str(tree)]
    subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL)
    before=digest(tree)
    subprocess.run(cmd,check=True,stdout=subprocess.DEVNULL)
    assert before==digest(tree), 'overlay not idempotent'
    # Missing anchor in an unapplied tree must not copy or modify any file.
    bad=tmp/'bad'; (bad/'bsd/dev').mkdir(parents=True); (bad/'bsd/dev/redwood_emmc.c').touch()
    (bad/'iokit/conf').mkdir(parents=True); (bad/'iokit/conf/files').write_text('incompatible\n')
    before=digest(bad)
    result=subprocess.run(['python3',str(ROOT/'xnu/duchamp-overlay.py'),str(bad)],capture_output=True)
    assert result.returncode and before==digest(bad)
    payload=(ROOT/'mac/hakios-kernel.sh').read_text().split('__DUCHAMP_OVERLAY__\n',1)[1]
    with tarfile.open(fileobj=io.BytesIO(base64.b64decode(payload)),mode='r:gz') as tar:
        for member in tar.getmembers():
            assert tar.extractfile(member).read()==(ROOT/member.name).read_bytes(), member.name
print('PASS: overlay idempotence, no changes on bad anchor, embedded payload matches source')
