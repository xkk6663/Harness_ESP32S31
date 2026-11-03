"""pytest 全局配置：把仓库根加入 sys.path，保证 `pc_host` 可被导入。"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
