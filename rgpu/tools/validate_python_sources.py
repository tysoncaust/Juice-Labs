from __future__ import annotations
import ast
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
FILES=[
 ROOT/'controller'/'rgpu_wire.py',
 ROOT/'controller'/'rgpu_controller.py',
 ROOT/'controller'/'rgpu_client_test.py',
 ROOT/'renderd'/'rgpu_agent.py',
 ROOT/'colab'/'rgpu_probe.py',
 ROOT/'colab'/'run_phase2_colab.py',
]
for path in FILES:
    ast.parse(path.read_text(encoding='utf-8-sig'), filename=str(path))
    print(f'PYTHON_AST_PASS={path.relative_to(ROOT)}')
print(f'PYTHON_SOURCE_VALIDATION=PASS files={len(FILES)}')
