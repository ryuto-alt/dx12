import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import flipbook as fb  # noqa: E402


@pytest.fixture(scope="session")
def quick_seq():
    """縮小版の煙と炎の系列(ループの混ぜ込み前)。セッション内で 1 回だけ焼く。"""
    st = fb.QUICK
    return {k: fb.bake_sequence(k, st) for k in ("fire", "smoke")}


@pytest.fixture(scope="session")
def dense_seq():
    """ブレンドの検証用: 1 ステップ刻みの系列(奇数枠が偶数枠どうしの中間の正解になる)。"""
    st = fb.Settings(grid=32, res=64, mv_res=32, cols=4, rows=3, spf=1, overlap=3, warmup=100)
    return st, {k: fb.bake_sequence(k, st, frames=33) for k in ("fire", "smoke")}
