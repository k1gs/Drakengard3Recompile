"""Inventory generated PPU code and unresolved lowering before a native boot."""
import argparse
import collections
import hashlib
import json
import re
from pathlib import Path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory',type=Path)
    p.add_argument('--out',type=Path,required=True)
    a=p.parse_args()
    if a.out.exists(): p.error('Choose a fresh report path')
    files=sorted(a.directory.glob('ppu_recomp_*.cpp'))
    if not files: p.error('No generated chunks')
    definitions, references=set(),set()
    findings=[]; todos=collections.Counter(); metadata=[]
    for file in files:
        raw=file.read_bytes(); text=raw.decode('utf-8',errors='replace')
        metadata.append(dict(name=file.name,size=len(raw),sha256=hashlib.sha256(raw).hexdigest()))
        definitions.update(re.findall(r'^void (func_[0-9A-Fa-f]+)\(ppu_context\* ctx\)\s*\{',text,re.M))
        references.update(re.findall(r'\b(func_[0-9A-Fa-f]+)\b',text))
        current=None
        for number,line in enumerate(text.splitlines(),1):
            match=re.match(r'void (func_[0-9A-Fa-f]+)\(',line)
            if match: current=match.group(1)
            if '/* TODO:' in line:
                body=line.split('/* TODO:',1)[1].split('*/',1)[0].strip()
                todos[body.split(' ',1)[0]]+=1
                findings.append(dict(file=file.name,line=number,function=current,kind='todo',detail=body))
            elif any(marker in line for marker in ('-> non-code','missing continuation','branch into undecoded','unmodeled')):
                findings.append(dict(file=file.name,line=number,function=current,kind='discarded-or-unmodeled',detail=line.strip()))
    missing=sorted(references-definitions)
    report=dict(chunks=metadata,definitions=len(definitions),references=len(references),undefined_symbols=missing,
                todo_mnemonics=dict(todos),findings=findings,
                caveat='Static inventory. A clean report is not proof of ISA semantics or game correctness.')
    a.out.write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(f'{len(files)} chunks, {sum(row["size"] for row in metadata)} bytes, {len(definitions)} definitions, {len(missing)} undefined function symbols')
    print(f'TODO mnemonics: {dict(todos)}; discarded/unmodeled sites: {len(findings)-sum(todos.values())}')
    return 1 if missing else 0


if __name__=='__main__': raise SystemExit(main())
