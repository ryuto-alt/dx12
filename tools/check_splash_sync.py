#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
起動音ミキサーのオフライン出力（SplashSyncDump が書く sync_*.wav / sync_*.json）を独立に検査する。

  tools\\build.ps1 -Tests のあとに:
    build\\release\\tests\\SplashSyncDump.exe <dir> [assets/editor/sounds/splash_d.wav]
    python tools/check_splash_sync.py <dir>

検査（WAV を PCM16 として読み直す = ミキサーの内部状態を信用しない）:
  1. 区間境界（開始 / 無音明け / ループの継ぎ目 / trigger / クロスフェード終わり / 終端）の不連続量:
       境界の前後 ±4 サンプルの「隣接差の最大」 <= 近傍(±40ms, 境界±4 を除く)の隣接差の最大 × 1.5
       → 段差（クリック）があれば近傍の 10 倍以上になるので確実に検出できる。
  2. ピークが -6 dBFS（PCM16 の量子化を見込んで ±0.3dB）を超えない・クリップしない。
  3. 頂点（hitOnsetFrame）が trigger + クロスフェード長 に来ている。
終了コード 0 = すべて合格。
"""
import json
import sys
import wave
from pathlib import Path

import numpy as np


def load(path: Path):
    with wave.open(str(path), "rb") as w:
        n, ch, sw, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        assert ch == 2 and sw == 2, "PCM16 ステレオのみ"
        x = np.frombuffer(w.readframes(n), dtype="<i2").reshape(-1, 2).astype(np.float64) / 32768.0
    return x, sr


def step_max(x: np.ndarray, lo: int, hi: int) -> float:
    lo = max(lo, 1)
    hi = min(hi, len(x))
    if hi <= lo:
        return 0.0
    return float(np.abs(np.diff(x[lo - 1:hi], axis=0)).max())


def check(path: Path) -> bool:
    x, sr = load(path)
    meta = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    ok = True
    peak = float(np.abs(x).max())
    peak_db = 20 * np.log10(peak + 1e-12)
    print(f"[{meta['scenario']}] {len(x)/sr:.3f}s  peak {peak_db:.2f} dBFS  hitOnset={meta['hitOnsetFrame']/sr:.3f}s  pop={meta['popFrame']/sr:.3f}s")
    if peak_db > -6.0 + 0.3:
        print("  NG: ピークが -6dBFS を超えています"); ok = False
    win = int(0.040 * sr)
    for b in meta["boundaries"]:
        f = int(b["frame"])
        near = step_max(x, f - 4, f + 5)
        ref = max(step_max(x, f - win, f - 4), step_max(x, f + 5, f + win))
        ratio = near / ref if ref > 0 else (0.0 if near == 0 else float("inf"))
        good = ratio <= 1.5
        ok &= good
        print(f"  {'OK' if good else 'NG'}  {b['name']:<24} @{f/sr:7.3f}s  境界の差 {near:.5f} / 近傍の最大 {ref:.5f}  = {ratio:.2f}")
    return ok


def main() -> int:
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    d = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r"C:\Users\ryuto\Documents\dx12-ui-audit\splash")
    files = sorted(d.glob("sync_*.wav"))
    if not files:
        print("sync_*.wav がありません（SplashSyncDump を先に実行）")
        return 2
    allok = True
    for f in files:
        allok &= check(f)
    print("結果:", "すべて合格（クリック無し）" if allok else "不合格あり")
    return 0 if allok else 1


if __name__ == "__main__":
    raise SystemExit(main())
