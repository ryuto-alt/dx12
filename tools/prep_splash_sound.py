#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
起動音 splash_d.mp3 を、エディタの起動画面が使う WAV + メタ情報に変換する（開発時に 1 回だけ実行）。

  python tools/prep_splash_sound.py
  python tools/prep_splash_sound.py --src <mp3> --out-dir <dir>

やること:
  1. ffmpeg（PATH 上）で 44.1kHz / ステレオ / float に復号
  2. ピークを -6 dBFS に正規化（元は -17 dBFS と小さい。システム音量に従う前提で「控えめ」にする）
  3. 先頭に短いフェードイン、末尾に長めのフェードアウト（余韻の終端でクリックを出さない）
  4. 頂点（最大の立ち上がり = onset）を自動検出し、メタ JSON に書く
        riser      = [0, onset)        … 頂点までの準備区間（実起動時間に合わせて長さを調整して再生する）
        hit + tail = [onset, 末尾)     … 頂点〜余韻
  5. splash_d.wav（PCM16）と splash_d.json を出力

出力:
  assets/editor/sounds/splash_d.wav
  assets/editor/sounds/splash_d.json   { onsetFrame, peakFrame, popOffsetFrames, ... }

★このファイルの出どころ/ライセンスは未確認で、リポジトリは PUBLIC。splash_d.* は .gitignore 済み。
   コミット・配布物（installer/build.ps1 も除外する）に入れないこと。
   実行時に WAV が無ければ、エンジンは埋め込み済みの案 A（src/core/SplashSoundData.h）へ自動で縮退する。

検出のしくみ:
  10ms RMS の包絡線（5ms ごと）を線形振幅のまま取り、「±100ms の差」が最大になる位置 = 頂点の立ち上がり。
  dB に直すと無音に近い区間の揺れを拾うので使わない（線形が「聞こえ方の盛り上がり」に近い）。
  検索範囲は 2.0s〜(全長-2.0s)。範囲外の小さな山を拾わないため。
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

SR = 44100
REPO = Path(__file__).resolve().parent.parent
SOUND_DIR = REPO / "assets" / "editor" / "sounds"

PEAK_DBFS = -6.0
FADE_IN_SEC = 0.020
TAIL_END_SEC = 8.50       # 元は 9.16s。-66dB まで落ちた 8.5s で切る
TAIL_FADE_SEC = 0.70
SEARCH_LO_SEC = 2.0
SEARCH_HI_MARGIN_SEC = 2.0
POP_OFFSET_MAX_SEC = 0.10  # 立ち上がり〜音量ピークの間で、視覚の「ポン」を合わせる位置（onset からの遅れ）


def decode(src: Path) -> np.ndarray:
    cmd = ["ffmpeg", "-v", "error", "-i", str(src), "-ar", str(SR), "-ac", "2", "-f", "f32le", "-"]
    p = subprocess.run(cmd, capture_output=True)
    if p.returncode != 0 or not p.stdout:
        sys.exit("ffmpeg で復号できません: " + p.stderr.decode("utf-8", "replace"))
    return np.frombuffer(p.stdout, dtype=np.float32).reshape(-1, 2).copy()


def envelope(x: np.ndarray, win_sec: float = 0.010, hop_sec: float = 0.005):
    m = x.mean(axis=1)
    win = int(win_sec * SR)
    hop = int(hop_sec * SR)
    n = (len(m) - win) // hop
    env = np.empty(n, dtype=np.float64)
    for i in range(n):
        seg = m[i * hop: i * hop + win]
        env[i] = np.sqrt(np.mean(seg * seg))
    return env, hop


def detect(x: np.ndarray):
    env, hop = envelope(x)
    total = len(x) / SR
    k = int(0.100 / (hop / SR))
    lo = int(SEARCH_LO_SEC / (hop / SR))
    hi = int(max(SEARCH_LO_SEC + 0.5, total - SEARCH_HI_MARGIN_SEC) / (hop / SR))
    best_i, best_r = lo, -1.0
    for i in range(lo, min(hi, len(env) - k)):
        r = env[i + k] - env[max(i - k, 0)]
        if r > best_r:
            best_i, best_r = i, r
    onset = best_i * hop / SR
    # 音量ピーク（onset の後ろ 0.8s 以内）
    a = best_i
    b = min(len(env), best_i + int(0.8 / (hop / SR)))
    pk_i = a + int(np.argmax(env[a:b]))
    peak = pk_i * hop / SR
    return onset, peak, best_r


def main() -> int:
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", type=Path, default=SOUND_DIR / "splash_d.mp3")
    ap.add_argument("--out-dir", type=Path, default=SOUND_DIR)
    args = ap.parse_args()

    if not args.src.exists():
        sys.exit(f"入力がありません: {args.src}")
    x = decode(args.src)
    print(f"復号: {len(x)/SR:.3f}s / {SR}Hz / stereo / peak {20*np.log10(np.abs(x).max()+1e-12):.2f} dBFS")

    # 末尾を切る → 正規化 → フェード
    end = min(len(x), int(TAIL_END_SEC * SR))
    x = x[:end]
    gain = 10.0 ** (PEAK_DBFS / 20.0) / float(np.abs(x).max())
    x = x * np.float32(gain)
    nfi = int(FADE_IN_SEC * SR)
    x[:nfi] *= (0.5 - 0.5 * np.cos(np.linspace(0.0, np.pi, nfi, dtype=np.float32)))[:, None]
    nfo = int(TAIL_FADE_SEC * SR)
    x[-nfo:] *= (0.5 + 0.5 * np.cos(np.linspace(0.0, np.pi, nfo, dtype=np.float32)))[:, None]

    onset, peak, rise = detect(x)
    pop_off = min(POP_OFFSET_MAX_SEC, max(0.0, (peak - onset) * 0.6))
    print(f"頂点の立ち上がり(onset) = {onset:.3f}s  音量ピーク = {peak:.3f}s  視覚のポン = onset+{pop_off*1000:.0f}ms  (rise={rise:.5f})")
    if not (3.0 <= onset <= 6.0):
        print("警告: onset が想定範囲(3〜6s)の外です。素材を差し替えた場合は確認してください。")

    pcm = np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2")
    args.out_dir.mkdir(parents=True, exist_ok=True)
    wav_path = args.out_dir / "splash_d.wav"
    with wave.open(str(wav_path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())

    meta = {
        "version": 1,
        "sampleRate": SR,
        "channels": 2,
        "frames": int(len(pcm)),
        "onsetFrame": int(round(onset * SR)),
        "peakFrame": int(round(peak * SR)),
        "popOffsetFrames": int(round(pop_off * SR)),
        "peakDbfs": PEAK_DBFS,
        "note": "riser=[0,onsetFrame) / hit+tail=[onsetFrame,frames)。源は splash_d.mp3（出どころ未確認・非コミット）",
    }
    (args.out_dir / "splash_d.json").write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    out_peak = 20 * np.log10(np.abs(pcm.astype(np.float32) / 32768.0).max())
    print(f"書き出し: {wav_path} ({len(pcm)/SR:.3f}s, peak {out_peak:.2f} dBFS)")
    print(f"メタ: {args.out_dir / 'splash_d.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
