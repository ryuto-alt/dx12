r"""ファイルの置き場。画像は巨大で PUBLIC リポジトリに入れないので、既定はリポジトリの外。

  %LOCALAPPDATA%\UnoEngine\parity\
      baselines\<sceneId>\{approved,pending}\<camera>.png (+ meta.json / history.jsonl)
      runs\<runId>\...                ランナーの出力(画像・metrics.json・report.html)
      noise\<sceneId>\<camera>.json   ノイズ床
      perf\<sceneId>.jsonl            性能履歴
      references\<sceneId>\<camera>.png   ユーザーが手動で撮った外部基準(UE 等)の既定の置き場

環境変数 PARITY_HOME で丸ごと差し替えできる(テストはこれで tmp に向ける)。
"""
from __future__ import annotations

import os
from pathlib import Path

# tools/parity/parity/paths.py -> リポジトリのルート
PKG_DIR = Path(__file__).resolve().parent
TOOL_DIR = PKG_DIR.parent
REPO_ROOT = TOOL_DIR.parent.parent


def home() -> Path:
    env = os.environ.get("PARITY_HOME")
    if env:
        return Path(env)
    la = os.environ.get("LOCALAPPDATA")
    if la:
        return Path(la) / "UnoEngine" / "parity"
    return Path.home() / ".local" / "share" / "UnoEngine" / "parity"


def baselines_dir() -> Path:
    return home() / "baselines"


def runs_dir() -> Path:
    return home() / "runs"


def noise_dir() -> Path:
    return home() / "noise"


def perf_dir() -> Path:
    return home() / "perf"


def references_dir() -> Path:
    return home() / "references"


def ensure(p: Path) -> Path:
    p.mkdir(parents=True, exist_ok=True)
    return p
