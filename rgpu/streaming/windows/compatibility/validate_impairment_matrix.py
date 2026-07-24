from __future__ import annotations
import argparse,json,sys
from pathlib import Path

def main()->int:
 p=argparse.ArgumentParser();p.add_argument('--profiles',required=True);p.add_argument('--evidence-dir',required=True);p.add_argument('--transport',choices=('srt','webrtc'),required=True);a=p.parse_args()
 profiles=json.loads(Path(a.profiles).read_text(encoding='utf-8-sig'));root=Path(a.evidence_dir);errors=[];passed=0
 for profile in profiles['profiles']:
  for seed in profiles['seeds']:
   path=root/f"{a.transport}-{profile['id']}-seed-{seed}.json"
   if not path.exists(): errors.append(f'missing:{path.name}');continue
   doc=json.loads(path.read_text(encoding='utf-8-sig'))
   if doc.get('duration_seconds',0)<profiles['minimum_duration_seconds']:errors.append(f'duration:{path.name}')
   if doc.get('seed')!=seed or doc.get('profile')!=profile['id']:errors.append(f'identity:{path.name}')
   for metric in profiles['required_metrics']:
    if metric not in doc:errors.append(f'metric:{metric}:{path.name}')
   if doc.get('result')!='pass':errors.append(f'result:{path.name}')
   else:passed+=1
 required=len(profiles['profiles'])*len(profiles['seeds'])
 print(f'IMPAIRMENT_MATRIX_TRANSPORT={a.transport.upper()}')
 print(f'IMPAIRMENT_MATRIX_PASSED={passed}/{required}')
 for e in errors:print(e,file=sys.stderr)
 return 0 if not errors and passed==required else 2
if __name__=='__main__':raise SystemExit(main())
