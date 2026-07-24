from __future__ import annotations
import argparse, hashlib, json, re, sys
from pathlib import Path
from uuid import UUID

def fail(msg: str) -> None: raise ValueError(msg)
def sha256(path: Path) -> str:
    h=hashlib.sha256()
    with path.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''): h.update(b)
    return h.hexdigest()
def validate(doc: dict) -> None:
    required=['schema_version','project_commit','binary_sha256','test_id','test_date_utc','operator','game','environment','results','artifacts','limitations']
    for key in required:
        if key not in doc: fail(f'missing:{key}')
    if doc['schema_version']!='1.0': fail('schema_version')
    if not re.fullmatch(r'[0-9a-f]{7,40}',doc['project_commit']): fail('project_commit')
    if not re.fullmatch(r'[0-9a-f]{64}',doc['binary_sha256']): fail('binary_sha256')
    UUID(doc['test_id'])
    if doc['operator'] not in ('human','automated-transport-only'): fail('operator')
    if doc.get('mode') not in ('passive','interactive'): fail('mode')
    env=doc['environment']
    for key in ('windows_build','gpu','driver','capture_backend','input_mode'):
        if key not in env: fail(f'environment.{key}')
    if env['input_mode'] not in ('disabled','software','hardware'): fail('environment.input_mode')
    claims=doc.get('claims',{})
    for key in ('vendor_approval','vendor_endorsement','universal_anti_cheat_compatibility'):
        if claims.get(key) is not False: fail(f'claims.{key}')
    for artifact in doc['artifacts']:
        if not re.fullmatch(r'[0-9a-f]{64}',artifact.get('sha256','')): fail('artifact.sha256')
        if artifact.get('media_retained') is not False: fail('artifact.media_retained')

def main() -> int:
    ap=argparse.ArgumentParser(); ap.add_argument('paths',nargs='+'); args=ap.parse_args()
    for raw in args.paths:
        path=Path(raw); doc=json.loads(path.read_text(encoding='utf-8-sig')); validate(doc); print(f'EVIDENCE_SCHEMA=PASS path={path}')
    return 0
if __name__=='__main__':
    try: raise SystemExit(main())
    except Exception as exc: print(f'EVIDENCE_SCHEMA=FAIL {exc}',file=sys.stderr); raise SystemExit(2)
