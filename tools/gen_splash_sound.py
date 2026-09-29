#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Uno Engine エディタ起動画面の「サウンドロゴ」を合成する（サンプル取得なし・著作権クリーン）。

出力:
  assets/editor/sounds/splash_a.wav  A: ぽん + きらめき上昇（既定の採用案）
  assets/editor/sounds/splash_b.wav  B: バブルポップ + ウッドブロック風の軽い 2 音
  assets/editor/sounds/splash_c.wav  C: 柔らかいベルの 3 音アルペジオ
  src/core/SplashSoundData.h         埋め込み用（--emit-header <a|b|c>、既定 a。モノラル 44.1kHz）
  <plot-dir>/splash_spectrogram.png  スペクトログラム（matplotlib があるときだけ）

再実行すると【同じ WAV】が出る（乱数は全てシード固定・numpy の default_rng）。
依存: numpy（必須）/ matplotlib（PNG のみ・無ければ省略）。scipy は使わない。

使い方:
  python tools/gen_splash_sound.py                    # 3 案 + 自己検査 + ヘッダ(a) + PNG
  python tools/gen_splash_sound.py --emit-header b    # 埋め込みを B に切り替える
  python tools/gen_splash_sound.py --no-plots         # PNG を作らない

音づくり（どれも「サイン波の羅列」にしない）:
  ・モーダル合成: 非整数倍音（マリンバ 1 : 3.92 : 9.24 / ガラス 1 : 2.76 : 5.40 / ベルは Risset 系）を
    それぞれ別の減衰時間で鳴らす。高次ほど早く消える＝叩いた音の質感。
  ・アタック直後の短い帯域ノイズ（マレットが当たる「コッ」）+ 微小なピッチのしなり(bend)と揺れ(vibrato)。
  ・左右で 0.04% ずらしたデチューン + 0.5ms 以内のハース遅延 + 左右別のノイズ IR のリバーブ
    （自作の減衰ノイズ・コンボリューション。高域ほど早く減衰する 2 バンド IR）。
"""
from __future__ import annotations

import argparse
import hashlib
import io
import sys
import wave
from pathlib import Path

import numpy as np

SR = 44100
LN1000 = 6.907755278982137          # exp(-t/tau) が -60dB になる t = tau * LN1000（T60 の換算）
REPO = Path(__file__).resolve().parent.parent
SOUND_DIR = REPO / "assets" / "editor" / "sounds"
HEADER_PATH = REPO / "src" / "core" / "SplashSoundData.h"
DEFAULT_PLOT_DIR = Path(r"C:\Users\ryuto\Documents\dx12-ui-audit\splash")

PEAK_TARGET_DB = -6.0               # 仕様は -3dBFS 以下。余裕を見て -6dBFS に揃える
FADE_IN_SEC = 0.006                 # 仕様 5ms 以上
LEAD_SEC = 0.006                    # 全イベントをフェードイン分だけ後ろへ（最初のアタックがフェードに食われない）
FADE_OUT_SEC = 0.120                # 仕様 80ms 以上
HEADER_MAX_BYTES = 100 * 1024

# ------------------------------------------------------------------ 部品（周波数領域フィルタ）


def _freqs(n: int) -> np.ndarray:
    return np.fft.rfftfreq(n, 1.0 / SR)


def fft_filter(x: np.ndarray, gain) -> np.ndarray:
    X = np.fft.rfft(x)
    return np.fft.irfft(X * gain(_freqs(len(x))), len(x))


def lowpass(x, fc):
    return fft_filter(x, lambda f: 1.0 / np.sqrt(1.0 + (f / fc) ** 4))


def highpass(x, fc):
    def g(f):
        with np.errstate(divide="ignore", invalid="ignore"):
            r = np.where(f > 0, 1.0 / np.sqrt(1.0 + (fc / np.maximum(f, 1e-9)) ** 4), 0.0)
        return r
    return fft_filter(x, g)


def bandpass_noise(rng, n, fc, width_oct=1.0):
    """fc を中心にした対数ガウス帯域のホワイトノイズ（RMS ≒ 1 に揃える）。"""
    noise = rng.standard_normal(n)

    def g(f):
        with np.errstate(divide="ignore", invalid="ignore"):
            r = np.where(f > 0, np.exp(-0.5 * (np.log2(np.maximum(f, 1e-9) / fc) / width_oct) ** 2), 0.0)
        return r
    y = fft_filter(noise, g)
    rms = np.sqrt(np.mean(y * y)) + 1e-12
    return y / rms


def add_at(buf: np.ndarray, start: int, sig: np.ndarray) -> None:
    if start >= len(buf):
        return
    end = min(len(buf), start + len(sig))
    buf[start:end] += sig[: end - start]


# ------------------------------------------------------------------ 部品（音源）

# (周波数比, 振幅, T60[秒])
MARIMBA = [(1.0, 1.00, 0.30), (3.92, 0.32, 0.10), (9.24, 0.07, 0.040)]
GLASS = [(1.0, 1.00, 0.55), (2.76, 0.38, 0.22), (5.40, 0.14, 0.090), (8.93, 0.05, 0.040)]
WOOD = [(1.0, 1.00, 0.12), (2.31, 0.55, 0.070), (4.02, 0.25, 0.045), (6.61, 0.10, 0.030)]
# Risset 系のベル（1.0 = ストライクトーン。0.5 = ハム）。1.0015 は極小デチューンのうなり用
BELL = [
    (0.50, 0.30, 1.10), (1.00, 1.00, 0.90), (1.0015, 0.45, 0.90), (1.19, 0.35, 0.70),
    (1.71, 0.22, 0.50), (2.00, 0.35, 0.55), (2.74, 0.15, 0.30), (3.76, 0.08, 0.18),
]
SUB = [(0.5, 0.55, 0.14)]           # 「ぽん」の胴鳴り


def modal(rng, freq, dur, partials, *, bend=0.0, bend_tau=0.02, vib=0.0, vib_rate=5.5,
          detune=0.0, attack=0.0015, tscale=1.0):
    """非整数倍音の減衰音。bend>0 で頭が高く落ちてくる（弾む）/ <0 で下から跳ね上がる。"""
    n = int(dur * SR)
    t = np.arange(n) / SR
    pm = 1.0 + bend * np.exp(-t / bend_tau)
    if vib:
        pm = pm * (1.0 + vib * np.sin(2 * np.pi * vib_rate * t + rng.uniform(0, 2 * np.pi))
                   * (1.0 - np.exp(-t / 0.12)))
    fm = freq * (1.0 + detune) * pm
    out = np.zeros(n)
    for ratio, amp, t60 in partials:
        if freq * ratio > 18000.0:
            continue
        ph = 2 * np.pi * np.cumsum(fm * ratio) / SR + rng.uniform(0, 2 * np.pi)
        tau = t60 * tscale / LN1000
        out += amp * np.sin(ph) * np.exp(-t / tau)
    out *= 1.0 - np.exp(-t / attack)
    return out


def click(rng, fc, dur, width_oct=1.0):
    """マレットが当たる瞬間のノイズ（短い帯域ノイズ + 急減衰）。"""
    n = max(8, int(dur * SR * 4))
    t = np.arange(n) / SR
    return bandpass_noise(rng, n, fc, width_oct) * np.exp(-t / (dur / 2.5)) * (1.0 - np.exp(-t / 0.0004))


def bubble(rng, f0, f1, tau_rise, dur, detune=0.0):
    """バブルポップ: 周波数が素早く上へ抜けていく sin + 2 倍音、急減衰。"""
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = (f0 + (f1 - f0) * (1.0 - np.exp(-t / tau_rise))) * (1.0 + detune)
    ph = 2 * np.pi * np.cumsum(f) / SR + rng.uniform(0, 2 * np.pi)
    env = (1.0 - np.exp(-t / 0.0008)) * np.exp(-t / 0.032)
    y = (np.sin(ph) + 0.18 * np.sin(2.0 * ph + 0.7)) * env
    return y


class Mix:
    """ステレオのドライバスに音を置いていく。"""

    def __init__(self, total_sec: float, pad_sec: float, seed: int):
        self.n = int((total_sec + pad_sec) * SR)
        self.buf = np.zeros((2, self.n))
        self.rng = np.random.default_rng(seed)

    def place(self, t0, voice_fn, *, pan=0.0, gain=1.0, haas_ms=0.5, det=0.0004,
              click_fc=0.0, click_amp=0.0, click_dur=0.006):
        """voice_fn(rng, detune) -> モノラル波形。左右で detune を ±det、遅延を pan に応じて微小にずらす。"""
        start = int(round((t0 + LEAD_SEC) * SR))
        ang = (pan + 1.0) * np.pi / 4.0                       # 等パワーパン
        gains = (np.cos(ang), np.sin(ang))
        for ch, sgn in ((0, -1.0), (1, +1.0)):
            sig = voice_fn(self.rng, sgn * det)
            if click_amp > 0.0:
                c = click(self.rng, click_fc, click_dur) * click_amp
                sig = sig.copy()
                sig[: len(c)] += c[: len(sig)]
            # ハース遅延: パンの反対側のチャンネルを最大 haas_ms だけ遅らせる
            far = (ch == 0 and pan > 0) or (ch == 1 and pan < 0)
            d = int(round(abs(pan) * haas_ms * 1e-3 * SR)) if far else 0
            add_at(self.buf[ch], start + d, sig * gains[ch] * gain)

    def shimmer(self, t0, dur, amp, fc=6500.0):
        """高域のきらめき粉（左右で別ノイズ）。"""
        n = int(dur * SR)
        t = np.arange(n) / SR
        env = (1.0 - np.exp(-t / 0.008)) * np.exp(-t / (dur / 3.5))
        for ch in (0, 1):
            y = highpass(bandpass_noise(self.rng, n, fc, 0.9), fc * 0.6) * env * amp
            add_at(self.buf[ch], int(round((t0 + LEAD_SEC) * SR)), y)


# ------------------------------------------------------------------ リバーブ / 仕上げ


def make_ir(rng, t60, length, predelay=0.008, split_hz=2500.0, hf_ratio=0.45):
    """左右別に作る減衰ノイズ IR。高域(split 以上)は t60*hf_ratio で早く消える。エネルギー = 1。"""
    n = int(length * SR)
    t = np.arange(n) / SR
    noise = rng.standard_normal(n)
    lo = lowpass(noise, split_hz)
    hi = noise - lo
    ir = lo * np.exp(-LN1000 * t / t60) + 0.7 * hi * np.exp(-LN1000 * t / (t60 * hf_ratio))
    ir *= 1.0 - np.exp(-t / 0.004)                            # 立ち上がりをなだらかに
    for tap_ms, a in ((11, 0.35), (17, -0.28), (23, 0.22), (31, -0.16)):   # 初期反射（少しだけ）
        k = int(tap_ms * 1e-3 * SR)
        if k < n:
            ir[k] += a * 0.05 * np.sqrt(n / SR)
    pre = int(predelay * SR)
    ir = np.concatenate([np.zeros(pre), ir])
    return ir / (np.sqrt(np.sum(ir * ir)) + 1e-12)


def fft_conv(x, ir):
    n = len(x) + len(ir) - 1
    nfft = 1 << (n - 1).bit_length()
    y = np.fft.irfft(np.fft.rfft(x, nfft) * np.fft.rfft(ir, nfft), nfft)
    return y[: len(x)]


def finish(mix: Mix, total_sec: float, *, wet, t60, ir_len, seed):
    rng = np.random.default_rng(seed)
    out = np.zeros_like(mix.buf)
    for ch in (0, 1):
        ir = make_ir(rng, t60, ir_len)
        out[ch] = mix.buf[ch] + wet * fft_conv(mix.buf[ch], ir)
        out[ch] = highpass(out[ch], 30.0)                     # DC / 超低域を除く
    n = int(round(total_sec * SR))
    out = out[:, :n]
    # フェード（先頭 6ms / 末尾 120ms の cos²。先頭・末尾サンプルは厳密に 0）
    fi = int(FADE_IN_SEC * SR)
    fo = int(FADE_OUT_SEC * SR)
    win_in = np.sin(0.5 * np.pi * np.arange(fi) / fi) ** 2
    win_out = np.cos(0.5 * np.pi * np.arange(fo) / (fo - 1)) ** 2
    out[:, :fi] *= win_in
    out[:, -fo:] *= win_out
    peak = np.max(np.abs(out))
    out *= (10 ** (PEAK_TARGET_DB / 20.0)) / peak
    return out.T.copy()                                       # (n, 2)


# ------------------------------------------------------------------ 3 案


def variant_a():
    """A: ぽん + きらめき上昇。C5 のマリンバ風「ぽん」→ G5・C6・E6 のガラスのきらめき。"""
    total = 0.92
    m = Mix(total, 0.6, seed=1101)
    # ぽん（弾む: 頭が 6% 高く落ちてくる）+ 胴鳴り
    m.place(0.0, lambda r, d: modal(r, 523.25, 0.5, MARIMBA, bend=0.06, bend_tau=0.03,
                                    vib=0.0012, detune=d, attack=0.0012),
            pan=0.0, gain=1.0, click_fc=3200, click_amp=0.20, click_dur=0.006)
    m.place(0.0, lambda r, d: modal(r, 523.25, 0.3, SUB, bend=0.10, bend_tau=0.025, detune=d, attack=0.0015),
            pan=0.0, gain=0.55)
    # 上昇するきらめき（下から跳ね上がる bend<0）
    for t0, f, pan, vel, ts in ((0.105, 783.99, -0.35, 0.50, 1.0),
                               (0.195, 1046.50, +0.30, 0.46, 1.0),
                               (0.285, 1318.51, -0.15, 0.58, 1.7)):
        m.place(t0, lambda r, d, f=f, ts=ts: modal(r, f, 0.7, GLASS, bend=-0.02, bend_tau=0.012,
                                                   vib=0.0015, vib_rate=6.0, detune=d, attack=0.0010, tscale=ts),
                pan=pan, gain=vel, click_fc=7000, click_amp=0.10, click_dur=0.003)
    m.shimmer(0.285, 0.45, 0.030)
    return finish(m, total, wet=0.36, t60=0.35, ir_len=0.55, seed=2101)


def variant_b():
    """B: バブルポップ + ウッドブロック風の 2 音（A5 → E6 の上行 5 度）。"""
    total = 0.80
    m = Mix(total, 0.5, seed=1202)
    m.place(0.0, lambda r, d: bubble(r, 480.0, 1250.0, 0.018, 0.16, detune=d),
            pan=-0.10, gain=1.30, det=0.004, click_fc=4500, click_amp=0.10, click_dur=0.004)
    m.place(0.135, lambda r, d: modal(r, 880.0, 0.3, WOOD + [(0.5, 0.30, 0.08)], bend=0.02, bend_tau=0.008,
                                      detune=d, attack=0.0006),
            pan=-0.25, gain=0.90, click_fc=2600, click_amp=0.32, click_dur=0.005)
    m.place(0.265, lambda r, d: modal(r, 1318.51, 0.4, WOOD + [(0.5, 0.25, 0.09)], bend=0.02, bend_tau=0.008,
                                      detune=d, attack=0.0006, tscale=1.8),
            pan=+0.25, gain=0.90, click_fc=2800, click_amp=0.30, click_dur=0.005)
    # 終止にガラスの薄い余韻（クリックだけで終わって唐突にならないように）
    m.place(0.265, lambda r, d: modal(r, 1318.51, 0.55, [(1.0, 1.0, 0.45), (2.76, 0.30, 0.18)],
                                      vib=0.0015, detune=d, attack=0.002),
            pan=+0.10, gain=0.24)
    return finish(m, total, wet=0.30, t60=0.25, ir_len=0.40, seed=2202)


def variant_c():
    """C: 柔らかいベルの 3 音アルペジオ（D5 → A5 → F#6）。"""
    total = 0.98
    m = Mix(total, 0.7, seed=1303)
    for t0, f, pan, vel, ts in ((0.000, 587.33, -0.30, 0.80, 1.0),
                               (0.140, 880.00, 0.00, 0.72, 1.0),
                               (0.280, 1479.98, +0.30, 0.86, 1.6)):
        m.place(t0, lambda r, d, f=f, ts=ts: modal(r, f, 0.85, BELL, bend=-0.006, bend_tau=0.02,
                                                   vib=0.0020, vib_rate=5.2, detune=d, attack=0.0030, tscale=ts),
                pan=pan, gain=vel, click_fc=2400, click_amp=0.07, click_dur=0.005)
    m.shimmer(0.280, 0.5, 0.018, fc=7500.0)
    return finish(m, total, wet=0.50, t60=0.55, ir_len=0.80, seed=2303)


VARIANTS = {
    "a": ("A: ぽん + きらめき上昇", variant_a),
    "b": ("B: バブルポップ + ウッドブロック", variant_b),
    "c": ("C: 柔らかいベルの 3 音アルペジオ", variant_c),
}

# ------------------------------------------------------------------ WAV 入出力


def to_int16(x: np.ndarray) -> np.ndarray:
    return np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2")


def wav_bytes(pcm: np.ndarray, sr: int) -> bytes:
    ch = 1 if pcm.ndim == 1 else pcm.shape[1]
    bio = io.BytesIO()
    with wave.open(bio, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.astype("<i2").tobytes())
    return bio.getvalue()


def resample_fft(x: np.ndarray, sr_new: int) -> np.ndarray:
    n = len(x)
    n_new = int(round(n * sr_new / SR))
    X = np.fft.rfft(x)
    Y = np.zeros(n_new // 2 + 1, dtype=complex)
    m = min(len(X), len(Y))
    Y[:m] = X[:m]
    return np.fft.irfft(Y, n_new) * (n_new / n)


# ------------------------------------------------------------------ 自己検査


def analyze(pcm: np.ndarray, sr: int, wav: bytes) -> dict:
    x = pcm.astype(np.float64) / 32768.0
    mono = x if x.ndim == 1 else x.mean(axis=1)
    n = len(mono)
    peak = float(np.max(np.abs(x)))
    rms = float(np.sqrt(np.mean(x * x)))
    ms = lambda v: max(1, int(v * 1e-3 * sr))
    head = float(np.max(np.abs(x[: ms(1.0)])))
    tail = float(np.max(np.abs(x[-ms(1.0):])))
    tail80 = float(np.max(np.abs(x[-ms(80.0):])))
    prev80 = float(np.max(np.abs(x[-ms(160.0):-ms(80.0)])))
    dc = float(abs(np.mean(x)))
    clip = int(np.sum(np.abs(pcm) >= 32767))
    step = float(np.max(np.abs(np.diff(x, axis=0)))) if n > 1 else 0.0
    db = lambda v: 20.0 * np.log10(max(v, 1e-9))
    checks = {
        "長さ 0.7-1.0s": 0.7 <= n / sr <= 1.0,
        "ピーク <= -3dBFS": db(peak) <= -3.0,
        "クリップ無し": clip == 0,
        "RMS <= ピーク-12dB": db(rms) <= db(peak) - 12.0,
        "先頭サンプル 0 かつ先頭1msが小さい(<=ピーク-20dB)": pcm.reshape(len(pcm), -1)[0].max() == 0 and head <= peak * 0.1,
        "末尾 0 近傍(<-60dB)": pcm.reshape(len(pcm), -1)[-1].max() == 0 and tail < 1e-3,
        "末尾80msが減衰中(直前80msより小さい)": tail80 < prev80 and tail80 <= 10 ** (-30 / 20),
        "DC <= -70dBFS": db(dc) <= -70.0,
        "隣接差 <= ピーク/2": step <= peak * 0.5,
    }
    return dict(sec=n / sr, sr=sr, ch=1 if x.ndim == 1 else x.shape[1], bytes=len(wav),
                peak_db=db(peak), rms_db=db(rms), crest=db(peak) - db(rms), clip=clip,
                head_db=db(head), tail_db=db(tail), tail80_db=db(tail80), dc_db=db(dc),
                step=step / max(peak, 1e-9), checks=checks, ok=all(checks.values()))


def print_report(name: str, r: dict) -> None:
    print(f"  [{name}] {'PASS' if r['ok'] else 'FAIL'}")
    for k, v in r["checks"].items():
        print(f"      {'ok ' if v else 'NG '} {k}")


def print_table(rows: dict) -> None:
    hdr = ["案", "長さ", "sr/ch", "bytes", "ピーク", "RMS", "クレスト", "クリップ", "先頭1ms", "末尾1ms",
           "末尾80ms", "DC", "判定"]
    print(" | ".join(hdr))
    print(" | ".join(["---"] * len(hdr)))
    for key, r in rows.items():
        print(" | ".join([
            key.upper(), f"{r['sec']:.3f}s", f"{r['sr']}/{r['ch']}", str(r["bytes"]),
            f"{r['peak_db']:.1f}dB", f"{r['rms_db']:.1f}dB", f"{r['crest']:.1f}dB", str(r["clip"]),
            f"{r['head_db']:.0f}dB", f"{r['tail_db']:.0f}dB", f"{r['tail80_db']:.0f}dB",
            f"{r['dc_db']:.0f}dB", "PASS" if r["ok"] else "FAIL"]))


# ------------------------------------------------------------------ ヘッダ / PNG


def emit_header(key: str, stereo: np.ndarray) -> dict:
    """埋め込み用: モノラル 44.1kHz（100KB を超えるなら 32kHz へ）。"""
    mono = stereo.mean(axis=1)
    mono *= (10 ** (PEAK_TARGET_DB / 20.0)) / np.max(np.abs(mono))
    sr = SR
    data = mono
    wav = wav_bytes(to_int16(data), sr)
    if len(wav) > HEADER_MAX_BYTES:
        sr = 32000
        data = resample_fft(mono, sr)
        data *= (10 ** (PEAK_TARGET_DB / 20.0)) / np.max(np.abs(data))
        data[:8] = 0.0  # 念のため先頭 0
        data[-1] = 0.0
        wav = wav_bytes(to_int16(data), sr)
    if len(wav) > HEADER_MAX_BYTES:
        raise SystemExit(f"埋め込み WAV が {HEADER_MAX_BYTES} バイトを超えました: {len(wav)}")
    lines = []
    for i in range(0, len(wav), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in wav[i:i + 16]) + ",")
    text = (
        f"// 自動生成: tools/gen_splash_sound.py --emit-header {key}\n"
        f"// 手で編集しないこと（変えたいときはスクリプトを直して再実行する）。\n"
        f"// 中身: 起動サウンドロゴ 案{key.upper()}（{VARIANTS[key][0]}）の WAV ファイル丸ごと\n"
        f"//       PCM 16bit / モノラル / {sr}Hz / {len(data) / sr:.3f} 秒 / {len(wav)} バイト。\n"
        f"//       合成音（サンプルの取得なし）。再生は PlaySoundW(SND_MEMORY) かエンジンのオーディオへメモリから渡す。\n"
        f"#pragma once\n\n#include <cstddef>\n\n"
        f"inline constexpr unsigned char kSplashSoundWav[] = {{\n" + "\n".join(lines) + "\n};\n\n"
        f"inline constexpr std::size_t kSplashSoundWavSize = sizeof(kSplashSoundWav);\n"
    )
    HEADER_PATH.parent.mkdir(parents=True, exist_ok=True)
    HEADER_PATH.write_text(text, encoding="utf-8", newline="\n")
    return dict(bytes=len(wav), sr=sr, sec=len(data) / sr, pcm=to_int16(data), wav=wav)


def plot_all(stereos: dict, plot_dir: Path) -> str:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:                                    # noqa: BLE001
        return f"省略（matplotlib なし: {e}）"
    plot_dir.mkdir(parents=True, exist_ok=True)
    fig, axes = plt.subplots(2, 3, figsize=(15, 6.2), gridspec_kw={"height_ratios": [1, 3]})
    nfft, hop = 1024, 128
    win = np.hanning(nfft)
    for col, (key, st) in enumerate(stereos.items()):
        x = st.mean(axis=1)
        t = np.arange(len(x)) / SR
        ax = axes[0][col]
        ax.plot(t, st[:, 0], lw=0.5, color="#2b6cb0")
        ax.plot(t, st[:, 1], lw=0.5, color="#dd6b20", alpha=0.7)
        ax.set_xlim(0, t[-1]); ax.set_ylim(-0.6, 0.6)
        ax.set_title(f"splash_{key}  ({len(x) / SR:.2f}s)  L/R", fontsize=10)
        ax.set_xticks([])
        frames = 1 + (len(x) - nfft) // hop
        S = np.stack([np.abs(np.fft.rfft(x[i * hop:i * hop + nfft] * win)) for i in range(frames)], axis=1)
        S_db = 20 * np.log10(S / S.max() + 1e-6)
        ax2 = axes[1][col]
        im = ax2.imshow(S_db, origin="lower", aspect="auto", cmap="magma", vmin=-90, vmax=0,
                        extent=[0, len(x) / SR, 0, SR / 2 / 1000])
        ax2.set_ylim(0, 16)
        ax2.set_xlabel("time [s]")
        if col == 0:
            ax2.set_ylabel("frequency [kHz]")
    fig.colorbar(im, ax=axes[1].tolist(), label="dB (rel. max)", fraction=0.02, pad=0.01)
    out = plot_dir / "splash_spectrogram.png"
    fig.savefig(out, dpi=110)
    plt.close(fig)
    return str(out)


# ------------------------------------------------------------------ main


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:                                         # noqa: BLE001
        pass
    ap = argparse.ArgumentParser(description="起動サウンドロゴを合成する")
    ap.add_argument("--emit-header", choices=sorted(VARIANTS), default="a",
                    help="SplashSoundData.h に埋め込む案（既定 a）")
    ap.add_argument("--no-header", action="store_true", help="ヘッダを書かない")
    ap.add_argument("--no-plots", action="store_true", help="スペクトログラム PNG を作らない")
    ap.add_argument("--plot-dir", default=str(DEFAULT_PLOT_DIR))
    args = ap.parse_args()

    SOUND_DIR.mkdir(parents=True, exist_ok=True)
    rows, stereos, all_ok = {}, {}, True
    print("== 合成 ==")
    for key, (title, fn) in VARIANTS.items():
        st = fn()
        st2 = fn()                                            # 決定論の確認（同じ入力→同じ出力）
        pcm = to_int16(st)
        h1 = hashlib.sha256(pcm.tobytes()).hexdigest()
        h2 = hashlib.sha256(to_int16(st2).tobytes()).hexdigest()
        wav = wav_bytes(pcm, SR)
        (SOUND_DIR / f"splash_{key}.wav").write_bytes(wav)
        r = analyze(pcm, SR, wav)
        r["checks"]["再実行で同一(sha256)"] = h1 == h2
        r["ok"] = all(r["checks"].values())
        rows[key], stereos[key] = r, st
        all_ok &= r["ok"]
        print(f"  {title}  sha256={h1[:12]}")
        print_report(key.upper(), r)

    print("\n== 自己検査（WAV ファイル）==")
    print_table(rows)

    if not args.no_header:
        info = emit_header(args.emit_header, stereos[args.emit_header])
        hr = analyze(info["pcm"], info["sr"], info["wav"])
        print(f"\n== 埋め込みヘッダ（案{args.emit_header.upper()}, モノラル）==")
        print(f"  {HEADER_PATH}")
        print(f"  {info['sr']}Hz / {info['sec']:.3f}s / WAV {info['bytes']} バイト（上限 {HEADER_MAX_BYTES}）"
              f" / ピーク {hr['peak_db']:.1f}dB / RMS {hr['rms_db']:.1f}dB / {'PASS' if hr['ok'] else 'FAIL'}")
        all_ok &= hr["ok"]

    if not args.no_plots:
        print("\n== スペクトログラム ==\n  " + plot_all(stereos, Path(args.plot_dir)))
    print("\n総合:", "PASS" if all_ok else "FAIL")
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
