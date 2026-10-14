"""Exercise the actual lifted startup prefix and verify fail-closed diagnostics."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe', type=Path, default=Path('work/startup-probe-005/startup_probe.exe'))
    p.add_argument('--elf', type=Path, default=Path('work/extracted/PS3_GAME/USRDIR/EBOOT.elf'))
    p.add_argument('--out', type=Path, default=Path('reports/startup-probe-check-001.json'))
    a = p.parse_args()
    if a.out.exists():
        p.error('Choose a fresh report name')
    cases = [(f'variant-{v}', [str(a.elf),str(v)],0,f'PASS variant={v}') for v in range(4)]
    cases += [('unmapped-toc',[str(a.elf),'0','unmapped-toc'],1,'FAIL Unexpected stop frontier'),
              ('wrong-register',[str(a.elf),'0','wrong-register'],1,'FAIL TLS arguments differ')]
    cases += [(f'table-helper-{v}',[str(a.elf),str(v),'table-helper'],0,f'PASS table_helper variant={v} iterations=253') for v in (0,3)]
    cases += [('reference-boot',[str(a.elf),'0','reference-boot'],0,'PASS reference_boot_scalars tls=0x197fc7c filesz=0x2e8 memsz=0x2e8')]
    results = []
    with tempfile.TemporaryDirectory(dir=a.probe.resolve().parent,prefix='probe-negative-') as folder:
        if not Path(folder).resolve().is_relative_to(a.probe.resolve().parent):
            raise ValueError('Temporary fixture escaped probe directory')
        modified = Path(folder)/'modified.elf'
        wrong_size = Path(folder)/'wrong-size.elf'
        wrong_size.write_bytes(b'NOT-ELF')
        cases.append(('wrong-elf',[str(wrong_size)],1,'FAIL ELF size mismatch'))
        altered = bytearray(a.elf.read_bytes())
        altered[0xa7a020] ^= 1
        modified.write_bytes(altered)
        cases.append(('modified-elf',[str(modified)],1,'FAIL ELF SHA256 mismatch'))
        for name,args,expected,marker in cases:
            run = subprocess.run([str(a.probe.resolve()),*args],capture_output=True,text=True,timeout=15)
            text = run.stdout+run.stderr
            passed = run.returncode==expected and marker in text
            if name.startswith('variant-') or name=='reference-boot':
                passed &= 'STOP kind=hle-import value=0x744680a2 lr=0xa8a180' in text
            results.append(dict(case=name,passed=bool(passed),exit_code=run.returncode,expected_exit=expected,output=text))
            print(f"{'PASS' if passed else 'FAIL'} {name}")
    a.out.parent.mkdir(parents=True,exist_ok=True)
    a.out.write_text(json.dumps(dict(cases=results,scope='Native lifted entry/CRT with synthetic boot registers, stopping before HLE; separately a self-contained table initializer.'),indent=2),encoding='utf-8')
    return 0 if all(row['passed'] for row in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
