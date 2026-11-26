#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
起動音 Calibrate のデータ化（W1-BD1 / BA-1）。

Graphite Gauge の起動サウンドロゴ「Calibrate」（試作 1）を、決定論で再生成し、エンジンが使う形に書き出す。
合成は numpy だけ・乱数は全てシード固定。音づくりの部品（FFT フィルタ・クリック・FM ベル・サブヒット・プレートリバーブ・
マスタリング）は試作の make.py と同じ式で、`variant_2`（Escapement）は持ち込まない。

1 回の合成で次を作る（assets/editor/sounds/calibrate/ と src/core/）:
  graphite_1.wav       full。速い経路（ロードが 24 クリックの間に終わる）でそのまま 1 ボイスで鳴らす完成した master
  graphite_1_rm.wav    Reduce Motion 版。指針の風切り（帯域ノイズ）の層だけを外し、同じ master 処理を通したもの
  stems/bed.wav        遅い経路のロック前だけに使う「ベッド」（エア + うなり + そのリバーブ。ステレオ。0〜ロック）。
                       events.json の loop 区間でループする（区間の長さは 55Hz の 22 周期ぴったり）
  stems/click_00..23.wav  遅い経路のロック前だけに使うクリック 24 個（パン前のモノラル。master のゲインを掛け済み）
  stems/lock_tail.wav  full の master の T_lock 以降を【サンプル単位で切り出した】もの（ステムから作らない）
  events.json          全イベントの時刻（サンプル位置と ms）・種類・パン・ゲイン
  src/core/SplashCalibrateEvents.h  events.json と同じ値の constexpr 表（同じ生成器から。手で時刻を書かない）

時刻は make.py の実際の配置（Bus.put が数える開始サンプル）から取る。表を手で書かない。

使い方（numpy の版は tools/requirements-audio.txt に固定。`python -I` は user site を見ないので venv で実行する）:
  python -I tools/gen_boot_sound_calibrate.py              全出力を書き出す
  python -I tools/gen_boot_sound_calibrate.py --check      再合成して検査（numpy が要る。ctest の CalibrateSoundCheck）
  python -I tools/gen_boot_sound_calibrate.py --check-assets  コミット済みの資産だけの検査（標準ライブラリだけ。ctest の CalibrateSoundAssets）
"""
from __future__ import annotations

import argparse
import io
import json
import math
import re
import struct
import sys
import wave
from pathlib import Path

try:
    import numpy as np
except ImportError:                                  # --check-assets は numpy 無しで動く
    np = None

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "assets" / "editor" / "sounds" / "calibrate"
STEM_DIR = OUT_DIR / "stems"
HEADER_PATH = ROOT / "src" / "core" / "SplashCalibrateEvents.h"
EDITOR_RC = ROOT / "resources" / "editor.rc"
APP_RC = ROOT / "resources" / "app.rc"

SR = 44100
PEAK_DB = -6.0
LUFS_TARGET = -20.0
FADE_IN = 0.005
LEAD = 0.008            # all events start after the fade-in
LN1000 = 6.907755278982137
SIDE_GAIN = 0.32        # detuned side layer for tonal width (M/S, mono-compatible)

# ベッドのループ区間（ms）。長さ 400ms = 17640 サンプル = 55Hz の 22 周期（うなりの 55/110/165Hz が継ぎ目で連続する）。
# エアの立ち上がり（400ms の指数）と、うなりの立ち上がり（600ms）が済んだ後ろ、ロック（1,628）より前に置く。
BED_LOOP_START_MS = 1200.0
BED_LOOP_END_MS = 1600.0
BED_LOOP_XFADE_MS = 40.0        # ランタイムが線形のクロスフェードでつなぐ長さ（うなりは位相が揃うので線形で和が一定）
HAAS_MS = 0.35                  # Bus.put の既定: |pan| に比例した片側の遅延の最大（ms）

ONSETS: list = []


def db(x):
    return 10.0 ** (x / 20.0)


# ------------------------------------------------------------------ FFT filters

def _f(n):
    return np.fft.rfftfreq(n, 1.0 / SR)


def fft_filter(x, g):
    X = np.fft.rfft(x)
    return np.fft.irfft(X * g(_f(len(x))), len(x))


def lowpass(x, fc, order=2):
    return fft_filter(x, lambda f: 1.0 / np.sqrt(1.0 + (f / fc) ** (2 * order)))


def highpass(x, fc, order=2):
    def g(f):
        ff = np.maximum(f, 1e-9)
        r = 1.0 / np.sqrt(1.0 + (fc / ff) ** (2 * order))
        r[0] = 0.0
        return r
    return fft_filter(x, g)


def bandpass(x, fc, q):
    """2nd-order-ish resonant bandpass magnitude (|H| of an RBJ BPF, peak gain 1)."""
    def g(f):
        ff = np.maximum(f, 1e-9)
        u = q * (ff / fc - fc / ff)
        r = 1.0 / np.sqrt(1.0 + u * u)
        r[0] = 0.0
        return r
    return fft_filter(x, g)


def pink(rng, n):
    w = rng.standard_normal(n)
    y = fft_filter(w, lambda f: 1.0 / np.sqrt(np.maximum(f, 20.0)))
    return y / (np.sqrt(np.mean(y * y)) + 1e-12)


def add_at(buf, start, sig):
    if start >= len(buf) or start + len(sig) <= 0:
        return
    s0 = max(0, start)
    end = min(len(buf), start + len(sig))
    buf[s0:end] += sig[s0 - start: end - start]


# ------------------------------------------------------------------ sources

def ratchet_click(rng, *, fc=3200.0, q=8.0, ping_f=2400.0, ping_db=-14.0, body=None,
                  cents=0.0, gate=0.004, tau=0.0012):
    """One ratchet tooth: noise burst (0.2ms attack, exp decay) -> resonant BPF, + short sine ping,
    + optional modal body [(freq, amp, tau)] for detent weight."""
    k = 2.0 ** (cents / 1200.0)
    n = 1024
    t = np.arange(n) / SR
    env = (1.0 - np.exp(-t / 0.0002)) * np.exp(-t / tau)
    env *= np.clip((gate + 0.002 - t) / 0.002, 0.0, 1.0)          # 4 ms gate with 2 ms taper
    burst = rng.standard_normal(n) * env
    burst = np.concatenate([burst, np.zeros(n)])
    y = lowpass(bandpass(burst, fc * k, q), 9000.0, order=2)
    y /= np.max(np.abs(y)) + 1e-12
    tt = np.arange(2 * n) / SR
    ping = np.sin(2 * np.pi * ping_f * k * tt + rng.uniform(0, 6.28)) \
        * (1.0 - np.exp(-tt / 0.0003)) * np.exp(-tt / 0.0018) * np.clip((0.008 - tt) / 0.002, 0, 1)
    y += db(ping_db) * ping
    if body:
        for f, a, tb in body:
            y += a * np.sin(2 * np.pi * f * k * tt + rng.uniform(0, 6.28)) \
                * (1.0 - np.exp(-tt / 0.0004)) * np.exp(-tt / tb)
    # end taper so the tail is exactly zero
    m = int(0.004 * SR)
    y[-m:] *= np.linspace(1, 0, m)
    return y


def latch_click(rng, dur=0.003, hp=2000.0):
    n = 2048
    t = np.arange(n) / SR
    env = (1.0 - np.exp(-t / 0.00015)) * np.exp(-t / (dur / 3.0)) * np.clip((dur * 2.5 - t) / 0.002, 0, 1)
    y = highpass(rng.standard_normal(n) * env, hp, order=2)
    y /= np.max(np.abs(y)) + 1e-12
    y[-200:] *= np.linspace(1, 0, 200)
    return y


def modal(rng, f0, dur, partials, attack=0.0008, detune=0.0):
    """partials: [(ratio, amp, tau_s)]"""
    n = int(dur * SR)
    t = np.arange(n) / SR
    y = np.zeros(n)
    for r, a, tau in partials:
        f = f0 * r * (1.0 + detune)
        if f > 17000:
            continue
        y += a * np.sin(2 * np.pi * f * t + rng.uniform(0, 6.28)) * np.exp(-t / tau)
    y *= 1.0 - np.exp(-t / attack)
    return y


def fm_bell(rng, fc, dur, *, ratio=3.5, i0=2.2, i1=0.3, i_tau=0.12, attack=0.003, tau=0.9,
            detune=0.0, release_at=None, release_len=0.5):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = fc * (1.0 + detune)
    idx = i1 + (i0 - i1) * np.exp(-t / i_tau)
    ph_m = 2 * np.pi * f * ratio * t + rng.uniform(0, 6.28)
    y = np.sin(2 * np.pi * f * t + idx * np.sin(ph_m) + rng.uniform(0, 6.28))
    # a whisper of the strike partial an octave+fifth up for glassiness
    y += 0.06 * np.sin(2 * np.pi * f * 3.0 * t) * np.exp(-t / 0.08)
    env = (1.0 - np.exp(-t / attack)) * np.exp(-t / tau)
    if release_at is not None:
        r0 = int(release_at * SR)
        rl = int(release_len * SR)
        w = np.ones(n)
        if r0 < n:
            seg = np.cos(0.5 * np.pi * np.clip(np.arange(n - r0) / rl, 0, 1)) ** 2
            w[r0:] = seg
        env *= w
    return y * env


def sub_hit(dur, f_start, f_end, sweep_tau, tau, drive=1.6, attack=0.002):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f = f_end + (f_start - f_end) * np.exp(-t / sweep_tau)
    ph = 2 * np.pi * np.cumsum(f) / SR
    y = np.sin(ph)
    y = np.tanh(drive * y) / np.tanh(drive)           # adds 3rd harmonic -> audible on small speakers
    env = (1.0 - np.exp(-t / attack)) * np.exp(-t / tau)
    env *= np.clip((dur - t) / 0.05, 0, 1)
    return y * env


# ------------------------------------------------------------------ stereo bus

class Bus:
    def __init__(self, total, seed):
        self.n = int(total * SR)
        self.dry = np.zeros((2, self.n))
        self.send = np.zeros((2, self.n))      # reverb send
        self.rng = np.random.default_rng(seed)
        self.onsets = []                       # scheduled transient onsets (s), for the click checker
        self.events = []                       # [BD1] 実際に置いたイベント（開始サンプルつき）。events.json の元

    def _log(self, start, ev):
        if ev is not None:
            self.events.append(dict(sample=start, **ev))

    def put(self, t0, sig, *, pan=0.0, gain=1.0, wet=0.18, haas_ms=0.35, sig_r=None, ev=None):
        start = int(round((t0 + LEAD) * SR))
        self.onsets.append(t0 + LEAD)
        self._log(start, ev)
        ang = (np.clip(pan, -1, 1) + 1.0) * np.pi / 4.0
        gl, gr = np.cos(ang), np.sin(ang)
        d = int(round(abs(pan) * haas_ms * 1e-3 * SR))
        sl = sig
        sr = sig if sig_r is None else sig_r
        dl = d if pan > 0 else 0
        dr = d if pan < 0 else 0
        for ch, s, g, dd in ((0, sl, gl, dl), (1, sr, gr, dr)):
            add_at(self.dry[ch], start + dd, s * g * gain)
            add_at(self.send[ch], start + dd, s * g * gain * wet)

    def put_stereo(self, t0, l, r, gain=1.0, wet=0.18, ev=None):
        start = int(round((t0 + LEAD) * SR))
        self._log(start, ev)
        for ch, s in ((0, l), (1, r)):
            add_at(self.dry[ch], start, s * gain)
            add_at(self.send[ch], start, s * gain * wet)


def plate_ir(rng, t60=1.2, predelay=0.012, damp_hz=6000.0, length=1.6):
    n = int(length * SR)
    t = np.arange(n) / SR
    w = rng.standard_normal(n)
    lo = lowpass(w, 500.0)
    hi = highpass(w, damp_hz)
    mid = w - lo - hi
    ir = 0.9 * lo * np.exp(-LN1000 * t / (t60 * 0.85)) \
        + mid * np.exp(-LN1000 * t / t60) \
        + 0.6 * hi * np.exp(-LN1000 * t / (t60 * 0.3))
    ir *= 1.0 - np.exp(-t / 0.002)
    ir *= np.clip((length - t) / 0.1, 0, 1)
    ir = np.concatenate([np.zeros(int(predelay * SR)), ir])
    return ir / (np.sqrt(np.sum(ir * ir)) + 1e-12)


def fft_conv(x, ir):
    n = len(x) + len(ir) - 1
    nfft = 1 << (n - 1).bit_length()
    return np.fft.irfft(np.fft.rfft(x, nfft) * np.fft.rfft(ir, nfft), nfft)[: len(x)]


# ------------------------------------------------------------------ loudness / mastering

def _biquad_mag(b, a, f):
    w = 2 * np.pi * f / SR
    z = np.exp(-1j * w)
    return np.abs((b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z))


def k_weight_gain(f):
    G, Q, fc = 3.999843853973347, 0.7071752369554196, 1681.974450955533
    A = 10 ** (G / 40.0)
    w0 = 2 * np.pi * fc / SR
    c, al = np.cos(w0), np.sin(w0) / (2 * Q)
    b = [A * ((A + 1) + (A - 1) * c + 2 * np.sqrt(A) * al), -2 * A * ((A - 1) + (A + 1) * c),
         A * ((A + 1) + (A - 1) * c - 2 * np.sqrt(A) * al)]
    a = [(A + 1) - (A - 1) * c + 2 * np.sqrt(A) * al, 2 * ((A - 1) - (A + 1) * c),
         (A + 1) - (A - 1) * c - 2 * np.sqrt(A) * al]
    h1 = _biquad_mag(b, a, f)
    fc2, Q2 = 38.13547087602444, 0.5003270373238773
    w0 = 2 * np.pi * fc2 / SR
    c, al = np.cos(w0), np.sin(w0) / (2 * Q2)
    b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]
    a = [1 + al, -2 * c, 1 - al]
    return h1 * _biquad_mag(b, a, f)


def lufs(x):
    """x: (2, n). BS.1770-ish integrated loudness with K-weighting applied in the FFT domain."""
    kw = np.stack([fft_filter(x[c], k_weight_gain) for c in range(2)])
    blk, hop = int(0.4 * SR), int(0.1 * SR)
    pw = []
    for s in range(0, kw.shape[1] - blk + 1, hop):
        pw.append(np.sum(np.mean(kw[:, s:s + blk] ** 2, axis=1)))
    pw = np.array(pw)
    if len(pw) == 0:
        return -99.0
    L = -0.691 + 10 * np.log10(pw + 1e-20)
    g = pw[L > -70]
    if len(g) == 0:
        return -99.0
    rel = -0.691 + 10 * np.log10(np.mean(g)) - 10
    g2 = pw[(L > -70) & (L > rel)]
    return float(-0.691 + 10 * np.log10(np.mean(g2)))


def limiter(x, ceil, look=0.0015, release=0.05):
    """Lookahead peak limiter (stereo-linked). Gain ramps down before peaks, releases exponentially."""
    L = max(1, int(look * SR))
    peak = np.max(np.abs(x), axis=0)
    need = np.minimum(1.0, ceil / np.maximum(peak, 1e-12))
    pad = np.concatenate([need, np.ones(L)])
    win = np.lib.stride_tricks.sliding_window_view(pad, L + 1)
    gmin = win.min(axis=1)[: len(need)]                     # min over [n, n+L]
    # release smoothing (one-pole, only upward)
    a = np.exp(-1.0 / (release * SR))
    g = np.empty_like(gmin)
    cur = 1.0
    gl = gmin.tolist()
    out = [0.0] * len(gl)
    for i, v in enumerate(gl):
        cur = v if v < cur else v + (cur - v) * a
        out[i] = cur
    g = np.array(out)
    # attack smoothing: backward box filter of length L+1 over the forward-min; stays <= required gain
    k = np.ones(L + 1) / (L + 1)
    g = np.convolve(np.concatenate([np.ones(L), g]), k, mode="valid")
    return x * g


def tape_sat(x, drive_db=1.5):
    d = db(drive_db)
    return np.tanh(x * d) / d


def linear_chain(bus: Bus, rev_seed, t60=1.2, wet_gain=1.0):
    """master の線形な部分（ドライ + プレートの残響 → 24Hz HPF → 17.5kHz LPF）。ステムの書き出しも同じ式を通す。"""
    rng = np.random.default_rng(rev_seed)
    out = np.zeros_like(bus.dry)
    for ch in (0, 1):
        ir = plate_ir(rng, t60=t60)
        out[ch] = bus.dry[ch] + wet_gain * fft_conv(bus.send[ch], ir)
        out[ch] = highpass(out[ch], 24.0, order=2)
        out[ch] = lowpass(out[ch], 17500.0, order=4)
    return out


def master(bus: Bus, total, *, rev_seed, t60=1.2, wet_gain=1.0, tail_fade=0.35, aux=None):
    out = linear_chain(bus, rev_seed, t60=t60, wet_gain=wet_gain)
    n = int(round(total * SR))
    out = out[:, :n]

    def fades(y):
        fi, fo = int(FADE_IN * SR), int(tail_fade * SR)
        y = y.copy()
        y[:, :fi] *= np.sin(0.5 * np.pi * np.arange(fi) / fi) ** 2
        y[:, -fo:] *= np.cos(0.5 * np.pi * np.arange(fo) / (fo - 1)) ** 2
        return y

    out = fades(out)
    if aux is not None:
        aux["lin"] = out                                   # 非線形（飽和・リミッタ・ゲイン）を通す前の線形な信号
    ceil = db(PEAK_DB)
    base = db(LUFS_TARGET - lufs(out))
    extra = 0.0
    for _ in range(8):
        y = tape_sat(out * base * db(extra), 1.5)
        y = limiter(y, ceil * 0.985)
        y = fades(y)
        y *= ceil / np.max(np.abs(y))                     # exact peak
        err = LUFS_TARGET - lufs(y)
        if abs(err) < 0.1:
            break
        extra += err
    ONSETS.append(list(bus.onsets))
    return y.T.copy()


# ------------------------------------------------------------------ shared layers

def layer_air(bus, total, t_lock, lpf=700.0, level=-34.0, seed_off=0):
    """A: pink noise -> LPF, 400 ms exp rise, ducks to 0.4 at T_lock, gone in the last 600 ms."""
    n = bus.n
    t = np.arange(n) / SR
    env = 1.0 - np.exp(-t / 0.13)
    duck = np.where(t < t_lock, 1.0, 0.4 + 0.6 * np.exp(-(t - t_lock) / 0.15))
    end = total - 0.6
    rel = np.clip((total - 0.05 - t) / (total - 0.05 - end), 0, 1) ** 2
    env = env * duck * rel
    for ch in (0, 1):
        r = np.random.default_rng(9100 + seed_off + ch)
        a = lowpass(pink(r, n), lpf, order=1) * env
        a /= 1.0
        bus.dry[ch] += a * db(level)
        bus.send[ch] += a * db(level) * 0.1


def layer_hum(bus, total, t_lock, *, f0=55.0, levels=(-22, -28, -34), start=0.1, attack=0.6,
              release=0.9, blip=(58.0, 0.04)):
    """B: graphite hum 55 + 110 + triangle 165 (LPF 400). Pitch blip at T_lock."""
    n = bus.n
    t = np.arange(n) / SR
    f = np.full(n, f0)
    i0 = int((t_lock + LEAD) * SR)
    bl = int(blip[1] * SR)
    f[i0:i0 + bl] = blip[0] + (f0 - blip[0]) * (np.arange(min(bl, n - i0)) / bl)
    ph = 2 * np.pi * np.cumsum(f) / SR
    tri = (2 / np.pi) * np.arcsin(np.sin(3 * ph))
    sig = db(levels[0]) * np.sin(ph) + db(levels[1]) * np.sin(2 * ph + 0.4) \
        + db(levels[2]) * lowpass(tri, 400.0)
    ts = t - start - LEAD
    env = np.where(ts < 0, 0.0, 1.0 - np.exp(-np.maximum(ts, 0) / (attack / 3.0)))
    tr = t - (t_lock + LEAD)
    env *= np.where(tr < 0, 1.0, np.cos(0.5 * np.pi * np.clip(tr / release, 0, 1)) ** 2)
    sig *= env
    # slight stereo width: 0.3 Hz chorus on the 110 partial
    bus.dry[0] += sig
    bus.dry[1] += sig * 0.98
    bus.send[0] += sig * 0.05
    bus.send[1] += sig * 0.05


def click_times(n_clicks, t0, t1, d_start, d_mid, d_end, mid_pos=0.6):
    """Interval profile: d_start -> d_mid (accelerate) -> d_end (decelerate), scaled into [t0, t1]."""
    u = np.linspace(0, 1, n_clicks - 1)
    d = np.where(u < mid_pos,
                 d_start + (d_mid - d_start) * (0.5 - 0.5 * np.cos(np.pi * u / mid_pos)),
                 d_mid + (d_end - d_mid) * (0.5 - 0.5 * np.cos(np.pi * (u - mid_pos) / (1 - mid_pos))))
    d *= (t1 - t0) / d.sum()
    return t0 + np.concatenate([[0.0], np.cumsum(d)])




# ------------------------------------------------------------------ Calibrate（試作 1 の variant_1。配置は make.py のまま）

T_LOCK = 1.62
TOTAL = 3.75
BELLS = [("D4", 293.66, -20.0, -0.28), ("A4", 440.00, -20.0, -0.12), ("E5", 659.26, -20.0, 0.05),
         ("F#5", 739.99, -20.0, 0.18), ("A5", 880.00, -24.0, 0.32)]
KIND_ORDER = ["duck", "click", "latch", "tok", "sub", "bell", "ping", "end"]


def gdb(g):
    return round(20.0 * math.log10(g), 3)


def synth_calibrate(sweep=True, aux=None):
    """variant_1 と同じ乱数の消費順・同じ配置。sweep=False は指針の風切りの層（帯域ノイズ）だけを外す
    （風切りは専用の乱数 31077 を使うので、外しても他の層の乱数は動かない）。"""
    total, T = TOTAL, T_LOCK
    bus = Bus(total + 0.2, seed=31001)
    rng = bus.rng
    layer_air(bus, total, T, lpf=700.0, level=-33.0)
    layer_hum(bus, total, T)

    # C: ratchet - 24 clicks, 120 -> 35 -> 90 ms, pan follows one needle revolution
    times = click_times(24, 0.26, T - 0.085, 0.120, 0.035, 0.090, mid_pos=0.62)
    clicks = []
    for i, tc in enumerate(times):
        ang = 2 * np.pi * (i + 1) / 24.0
        pan = 0.6 * np.sin(ang)
        g = db(-24.0 + 11.0 + rng.uniform(-0.5, 0.5)) * (0.75 + 0.25 * np.sin(np.pi * i / 23))
        c = ratchet_click(rng, cents=rng.uniform(-40, 40),
                          body=[(1180.0, 0.22, 0.0025), (5150.0, 0.10, 0.0012)])
        bus.put(tc, c, pan=pan, gain=g, wet=0.06,
                ev=dict(kind="click", index=i, pan=round(float(pan), 4), gainDb=gdb(g), angleDeg=15 * (i + 1)))
        clicks.append((c, float(g)))

    sweep_span = None
    if sweep:
        # needle sweep: faint band-noise "air of motion" tracking the needle speed + pan
        n = int((T - 0.1) * SR)
        tt = np.arange(n) / SR
        sp = np.interp(tt, times[1:], 1.0 / np.diff(times))       # clicks/sec
        sp = np.convolve(sp, np.ones(2000) / 2000, mode="same")
        nz = np.random.default_rng(31077).standard_normal(n)
        sw = bandpass(nz, 2600.0, 1.4) * (sp / sp.max()) ** 1.5
        sw *= (1.0 - np.exp(-tt / 0.08)) * np.clip((T - 0.1 - tt) / 0.06, 0, 1)
        ang_c = np.interp(tt, times, 2 * np.pi * np.arange(1, 25) / 24.0)
        pan_c = 0.6 * np.sin(ang_c)
        gl = np.cos((pan_c + 1) * np.pi / 4)
        gr = np.sin((pan_c + 1) * np.pi / 4)
        bus.put_stereo(0.0, sw * gl, sw * gr, gain=db(-41.0), wet=0.10)
        s0 = int(round(LEAD * SR))
        sweep_span = (s0, s0 + n)

    # D: latch double click + metal tok
    bus.put(T, latch_click(rng), pan=0.0, gain=db(-15.0), wet=0.15,
            ev=dict(kind="latch", index=0, pan=0.0, gainDb=-15.0))
    bus.put(T + 0.018, latch_click(rng), pan=0.05, gain=db(-19.0), wet=0.15,
            ev=dict(kind="latch", index=1, pan=0.05, gainDb=-19.0))
    tok = modal(rng, 1200.0, 0.25, [(1.0, 1.0, 0.030), (2.75 / 1.2, 0.8, 0.026),
                                   (4.31 / 1.2, 0.25, 0.012)], attack=0.0004)
    bus.put(T + 0.002, tok, gain=db(-17.0), wet=0.2, ev=dict(kind="tok", index=0, pan=0.0, gainDb=-17.0))

    # confident sub hit at lock
    bus.put(T, sub_hit(0.9, 115.0, 50.0, 0.035, 0.22, drive=1.8), gain=db(-9.5), wet=0.03,
            ev=dict(kind="sub", index=0, pan=0.0, gainDb=-9.5, note="sub 115>50Hz"))
    thump = modal(rng, 190.0, 0.3, [(1.0, 1.0, 0.045), (2.3, 0.3, 0.02)], attack=0.001)
    bus.put(T, thump, gain=db(-17.0), wet=0.05,
            ev=dict(kind="sub", index=1, pan=0.0, gainDb=-17.0, note="thump 190Hz"))

    # E: calibration chime - FM bell D add9 strum, low -> high every 25 ms
    for k, (name, f, gdb_, p) in enumerate(BELLS):
        dur = 2.05
        # mono-safe width: shared centre note (panned) + small detuned side layer (+S / -S).
        c = fm_bell(rng, f, dur, release_at=1.45, release_len=0.6)
        s = SIDE_GAIN * fm_bell(rng, f, dur, detune=+3 / 1731.0, release_at=1.45, release_len=0.6)
        ang = (p + 1) * np.pi / 4
        bus.put_stereo(T + 0.020 + 0.025 * k, c * np.cos(ang) * 1.41 + s, c * np.sin(ang) * 1.41 - s,
                       gain=db(gdb_ + 6.0), wet=0.18,
                       ev=dict(kind="bell", index=k, pan=p, gainDb=round(gdb_ + 6.0, 3), note=f"{name} {f}Hz"))

    # F: registration ping (A6) at T+520ms
    ping = modal(rng, 1760.0, 0.2, [(1.0, 1.0, 0.025), (2.76, 0.12, 0.010)], attack=0.001)
    bus.put(T + 0.52, ping, gain=db(-30.0 + 8.0), wet=0.25,
            ev=dict(kind="ping", index=0, pan=0.0, gainDb=-22.0, note="A6 1760Hz"))

    # 層Aの duck（ロックでエアを 0.4 倍へ。layer_air が t >= T で切り替える最初のサンプル）と、終端
    duck_sample = int(np.argmax(np.arange(bus.n) / SR >= T))
    bus.events.append(dict(sample=duck_sample, kind="duck", index=0, pan=0.0, gainDb=gdb(0.4),
                           note="air bed x0.4, tau 150ms"))
    n_total = int(round(total * SR))
    bus.events.append(dict(sample=n_total, kind="end", index=0, pan=0.0, gainDb=0.0))

    y = master(bus, total, rev_seed=31500, t60=1.2, tail_fade=0.40, aux=aux)
    return dict(y=y, bus=bus, clicks=clicks, sweep_span=sweep_span, lock_sample=int(round((T + LEAD) * SR)),
                n_total=n_total)


# ------------------------------------------------------------------ WAV / 出力

def to_i16(x):
    return np.clip(np.round(x * 32767.0), -32768, 32767).astype("<i2")


def wav_bytes(pcm):
    """pcm: int16 の (n,) モノ または (n,2) ステレオ。wave モジュールの固定のヘッダ（バイト一致する）。"""
    ch = 1 if pcm.ndim == 1 else pcm.shape[1]
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(np.ascontiguousarray(pcm).astype("<i2").tobytes())
    return buf.getvalue()


def ms_of(sample):
    return round(sample * 1000.0 / SR, 4)


def build_outputs():
    """全出力を {リポジトリからの相対パス: bytes} と、検査用の中間値で返す。"""
    aux = {}
    full = synth_calibrate(True, aux)
    rm = synth_calibrate(False)
    y = full["y"]
    tl = full["lock_sample"]
    pcm_full = to_i16(y)
    pcm_rm = to_i16(rm["y"])

    # ステムのゲイン: master の非線形（飽和・リミッタ・ラウドネス・ピーク正規化）の、ロック前の実効ゲインを最小二乗で求める。
    # ロック前は小さな信号（飽和もリミッタも浅い）なので、線形な信号に 1 つのスカラーを掛けた形でよく合う。
    lin = aux["lin"][:, :tl]
    g_lin = float(np.sum(y[:tl].T * lin) / np.sum(lin * lin))

    # ベッド = エア + うなり（+ それらのリバーブ）だけを同じ線形の鎖に通したもの。ロックまで。ステレオ
    bb = Bus(TOTAL + 0.2, seed=31001)
    layer_air(bb, TOTAL, T_LOCK, lpf=700.0, level=-33.0)
    layer_hum(bb, TOTAL, T_LOCK)
    bed = linear_chain(bb, 31500)[:, :tl].copy()
    fi = int(FADE_IN * SR)
    bed[:, :fi] *= np.sin(0.5 * np.pi * np.arange(fi) / fi) ** 2
    pcm_bed = to_i16(bed.T * g_lin)

    files = {}
    rel = lambda p: p.relative_to(ROOT).as_posix()
    files[rel(OUT_DIR / "graphite_1.wav")] = wav_bytes(pcm_full)
    files[rel(OUT_DIR / "graphite_1_rm.wav")] = wav_bytes(pcm_rm)
    files[rel(STEM_DIR / "bed.wav")] = wav_bytes(pcm_bed)
    click_pcm = []
    for i, (c, g) in enumerate(full["clicks"]):
        pcm = to_i16(c * g * g_lin)
        click_pcm.append(pcm)
        files[rel(STEM_DIR / f"click_{i:02d}.wav")] = wav_bytes(pcm)
    files[rel(STEM_DIR / "lock_tail.wav")] = wav_bytes(pcm_full[tl:])        # full の master の T_lock 以降をサンプル単位で切り出す

    evs = sorted(full["bus"].events, key=lambda e: (e["sample"], KIND_ORDER.index(e["kind"]), e["index"]))
    events = []
    for e in evs:
        o = dict(tMs=ms_of(e["sample"]), sample=int(e["sample"]), kind=e["kind"], index=int(e["index"]),
                 pan=e["pan"], gainDb=e["gainDb"])
        if "angleDeg" in e:
            o["angleDeg"] = e["angleDeg"]
        if "note" in e:
            o["note"] = e["note"]
        events.append(o)
    ss = full["sweep_span"]
    doc = dict(
        format=1, sampleRate=SR, lengthMs=ms_of(full["n_total"]), lengthSamples=full["n_total"],
        tLockMs=ms_of(tl), tLockSample=tl,
        panLaw="equal-power: L=cos((pan+1)*pi/4), R=sin((pan+1)*pi/4); pan>0 delays L, pan<0 delays R by |pan|*haasMs",
        haasMs=HAAS_MS,
        stemGainDb=round(20.0 * math.log10(g_lin), 3),
        sweep=dict(startMs=ms_of(ss[0]), endMs=ms_of(ss[1]),
                   note="band noise tracking needle speed. absent in graphite_1_rm.wav (Reduce Motion)"),
        loop=dict(bedStartMs=BED_LOOP_START_MS, bedEndMs=BED_LOOP_END_MS, crossfadeMs=BED_LOOP_XFADE_MS),
        events=events)
    files[rel(OUT_DIR / "events.json")] = (json.dumps(doc, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
    files[rel(HEADER_PATH)] = render_header(doc).encode("utf-8")
    inter = dict(pcm_full=pcm_full, pcm_rm=pcm_rm, pcm_bed=pcm_bed, click_pcm=click_pcm, doc=doc, tl=tl, aux_ok=True)
    return files, inter


def chunked(items, per=6):
    return ["    " + ", ".join(items[i:i + per]) + ("," if i + per < len(items) else "")
            for i in range(0, len(items), per)]


def render_header(doc):
    clicks = [e for e in doc["events"] if e["kind"] == "click"]
    f = lambda x: repr(float(x))
    kinds = ["Duck", "Click", "Latch", "Tok", "Sub", "Bell", "Ping", "End"]
    L = []
    L.append("// 自動生成: tools/gen_boot_sound_calibrate.py（W1-BD1）。手で編集しないこと（直すときは生成器を直して再実行する）。")
    L.append("// 出典: 起動音 Calibrate（graphite_1.wav）の実際の配置。assets/editor/sounds/calibrate/events.json と同じ値。")
    L.append("// 時刻は graphite_1.wav の先頭からの ms（make.py の LEAD = 8ms を含む）。sample は 44.1kHz のサンプル位置。")
    L.append("#pragma once")
    L.append("")
    L.append("#include <cstddef>")
    L.append("#include <cstdint>")
    L.append("")
    L.append("namespace dx12e::splash::calibrate")
    L.append("{")
    L.append("")
    L.append(f"inline constexpr int kSampleRate = {doc['sampleRate']};")
    L.append(f"inline constexpr double kLengthMs = {f(doc['lengthMs'])};")
    L.append(f"inline constexpr int kLengthSamples = {doc['lengthSamples']};")
    L.append(f"inline constexpr double kLockMs = {f(doc['tLockMs'])};      // T_lock（ロック。ラッチ 1 / サブヒット）")
    L.append(f"inline constexpr int kLockSample = {doc['tLockSample']};")
    L.append(f"inline constexpr double kHaasMs = {f(doc['haasMs'])};         // パンの片側の遅延の最大（|pan| に比例）")
    L.append(f"inline constexpr double kSweepStartMs = {f(doc['sweep']['startMs'])};   // 指針の風切りの層（Reduce Motion では無し）")
    L.append(f"inline constexpr double kSweepEndMs = {f(doc['sweep']['endMs'])};")
    L.append(f"inline constexpr double kBedLoopStartMs = {f(doc['loop']['bedStartMs'])};   // stems/bed.wav のループ区間")
    L.append(f"inline constexpr double kBedLoopEndMs = {f(doc['loop']['bedEndMs'])};")
    L.append(f"inline constexpr double kBedLoopCrossfadeMs = {f(doc['loop']['crossfadeMs'])};")
    L.append("")
    L.append(f"inline constexpr int kClickCount = {len(clicks)};")
    L.append("// ラチェット C00〜C23 の時刻（ms）と、そのときの指針の角度（度。15 x (i + 1)）。パンは kEvents の pan")
    L.append("inline constexpr double kClickMs[kClickCount] = {")
    L += chunked([f(c["tMs"]) for c in clicks])
    L.append("};")
    L.append("inline constexpr double kClickAngleDeg[kClickCount] = {")
    L += chunked([f(c["angleDeg"]) for c in clicks])
    L.append("};")
    L.append("")
    L.append("enum class EventKind : std::uint8_t")
    L.append("{")
    L.append("    " + ", ".join(kinds))
    L.append("};")
    L.append("")
    L.append("struct Event")
    L.append("{")
    L.append("    double tMs;")
    L.append("    int sample;")
    L.append("    EventKind kind;")
    L.append("    int index;")
    L.append("    double pan;      // -1..1（パン前の値。constant-power の則は events.json の panLaw）")
    L.append("    double gainDb;   // master 処理の前のゲイン（参考値）")
    L.append("    const char* note;")
    L.append("};")
    L.append("")
    L.append("// 時刻の昇順（同時刻は Duck, Click, Latch, Tok, Sub, Bell, Ping, End の順）")
    L.append("inline constexpr Event kEvents[] = {")
    for e in doc["events"]:
        k = e["kind"].capitalize()
        L.append(f'    {{{f(e["tMs"])}, {e["sample"]}, EventKind::{k}, {e["index"]}, {f(e["pan"])}, '
                 f'{f(e["gainDb"])}, "{e.get("note", "")}"}},')
    L.append("};")
    L.append("inline constexpr std::size_t kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);")
    L.append("")
    L.append("} // namespace dx12e::splash::calibrate")
    return "\n".join(L) + "\n"


# ------------------------------------------------------------------ 検査（標準ライブラリだけの部分）

# §2.6 の「Calibrate キュー表」（EXEC_W05_W1）の時刻。検査専用（生成には使わない。生成は実際の配置から）
REF_CLICKS_MS = [268, 359, 449, 536, 620, 698, 770, 834, 892, 942, 985, 1022, 1054, 1083, 1110, 1136, 1166, 1200,
                 1241, 1289, 1346, 1408, 1475, 1543]
REF_MS = {"lock": 1628.0, "tok": 1630.0, "latch1": 1646.0, "ping": 2148.0,
          "bells": [1648.0, 1673.0, 1698.0, 1723.0, 1748.0]}
REF_TOL_MS = 0.5

RC_NAMES = ["UNO_CALIBRATE_FULL", "UNO_CALIBRATE_RM", "UNO_CALIBRATE_BED", "UNO_CALIBRATE_LOCK_TAIL"] + \
           [f"UNO_CALIBRATE_CLICK_{i:02d}" for i in range(24)]
RC_FILES = {"UNO_CALIBRATE_FULL": "graphite_1.wav", "UNO_CALIBRATE_RM": "graphite_1_rm.wav",
            "UNO_CALIBRATE_BED": "stems/bed.wav", "UNO_CALIBRATE_LOCK_TAIL": "stems/lock_tail.wav",
            **{f"UNO_CALIBRATE_CLICK_{i:02d}": f"stems/click_{i:02d}.wav" for i in range(24)}}


class CheckLog:
    def __init__(self):
        self.fail = 0

    def ok(self, cond, msg):
        print(f"   {'ok' if cond else 'NG'}  {msg}")
        if not cond:
            self.fail += 1
        return cond


def read_wav_i16(path_or_bytes):
    """標準ライブラリだけ: (channels, [interleaved int16 の array])"""
    import array
    src = io.BytesIO(path_or_bytes) if isinstance(path_or_bytes, (bytes, bytearray)) else str(path_or_bytes)
    with wave.open(src, "rb") as w:
        assert w.getframerate() == SR and w.getsampwidth() == 2, "44.1kHz / 16bit 以外"
        ch = w.getnchannels()
        a = array.array("h")
        a.frombytes(w.readframes(w.getnframes()))
        if sys.byteorder == "big":
            a.byteswap()
    return ch, a


def db_of_peak(v):
    return 20.0 * math.log10(max(v, 1e-12) / 32768.0)


def parse_header(text):
    text = text.replace("\r\n", "\n")
    def arr(name):
        m = re.search(name + r"\[kClickCount\] = \{\s*([^}]*)\}", text)
        return [float(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip()]
    ev = []
    for m in re.finditer(r'\{([-\d.eE+]+), (\d+), EventKind::(\w+), (-?\d+), ([-\d.eE+]+), ([-\d.eE+]+), "([^"]*)"\}', text):
        ev.append(dict(tMs=float(m.group(1)), sample=int(m.group(2)), kind=m.group(3).lower(), index=int(m.group(4)),
                       pan=float(m.group(5)), gainDb=float(m.group(6)), note=m.group(7)))
    num = lambda name: float(re.search(name + r" = ([-\d.eE+]+);", text).group(1))
    return dict(clickMs=arr("kClickMs"), clickAngle=arr("kClickAngleDeg"), events=ev, lockMs=num("kLockMs"),
                lengthMs=num("kLengthMs"), lockSample=int(num("kLockSample")), loopStart=num("kBedLoopStartMs"),
                loopEnd=num("kBedLoopEndMs"))


def check_events(log, doc, header_text):
    ev = doc["events"]
    clicks = [e for e in ev if e["kind"] == "click"]
    log.ok(len(clicks) == 24, "click が 24 個")
    ts = [e["tMs"] for e in ev]
    log.ok(ts == sorted(ts), "events.json の時刻が昇順")
    worst = max(abs(c["tMs"] - r) for c, r in zip(clicks, REF_CLICKS_MS))
    log.ok(len(clicks) == 24 and worst <= REF_TOL_MS, f"クリックの時刻が §2.6 の表と ±{REF_TOL_MS}ms 以内（最大 {worst:.3f}ms）")
    log.ok(all(abs(c["angleDeg"] - 15 * (i + 1)) < 1e-9 for i, c in enumerate(clicks)), "指針の角度 = 15 x (i + 1) 度（C23 で 360）")
    one = lambda kind, idx=0: next(e for e in ev if e["kind"] == kind and e["index"] == idx)
    pairs = [("ロック（latch 0）", one("latch", 0)["tMs"], REF_MS["lock"]), ("tok", one("tok")["tMs"], REF_MS["tok"]),
             ("latch 1", one("latch", 1)["tMs"], REF_MS["latch1"]), ("ping", one("ping")["tMs"], REF_MS["ping"])] + \
            [(f"bell {k}", one("bell", k)["tMs"], REF_MS["bells"][k]) for k in range(5)]
    worst = max(abs(a - b) for _, a, b in pairs)
    log.ok(worst <= REF_TOL_MS, f"ロック・tok・latch 1・bell x5・ping が §2.6 の表と ±{REF_TOL_MS}ms 以内（最大 {worst:.3f}ms）")
    log.ok(abs(doc["tLockMs"] - REF_MS["lock"]) <= REF_TOL_MS, "tLockMs = 1,628")
    log.ok(abs(doc["lengthMs"] - 3750.0) <= 0.1, "全長 3,750ms")
    # ヘッダ ↔ JSON
    h = parse_header(header_text)
    d = 0.0
    d = max([d] + [abs(a - c["tMs"]) for a, c in zip(h["clickMs"], clicks)])
    log.ok(len(h["clickMs"]) == 24 and len(h["events"]) == len(ev), "ヘッダの click / event の個数が JSON と同じ")
    for a, b in zip(h["events"], ev):
        d = max(d, abs(a["tMs"] - b["tMs"]))
        if a["sample"] != b["sample"] or a["kind"] != b["kind"] or a["index"] != b["index"]:
            d = 1e9
    d = max(d, abs(h["lockMs"] - doc["tLockMs"]), abs(h["lengthMs"] - doc["lengthMs"]))
    log.ok(d <= 0.05, f"ヘッダと JSON の時刻の差 {d:.4f}ms（0.05ms 以下）")
    log.ok(h["lockSample"] == doc["tLockSample"], "ヘッダの kLockSample が JSON と同じ")


def check_assets(log):
    """コミット済みの資産だけを見る検査（標準ライブラリだけ）。"""
    print("== calibrate assets (stdlib)")
    names = ["graphite_1.wav", "graphite_1_rm.wav", "stems/bed.wav", "stems/lock_tail.wav", "events.json"] + \
            [f"stems/click_{i:02d}.wav" for i in range(24)]
    miss = [n for n in names if not (OUT_DIR / n).is_file()]
    log.ok(not miss, f"資産が揃っている（欠け: {miss}）")
    log.ok(HEADER_PATH.is_file(), "src/core/SplashCalibrateEvents.h がある")
    if miss or not HEADER_PATH.is_file():
        return
    doc = json.loads((OUT_DIR / "events.json").read_text(encoding="utf-8"))
    n_total, tl = doc["lengthSamples"], doc["tLockSample"]
    ch_f, full = read_wav_i16(OUT_DIR / "graphite_1.wav")
    ch_r, rm = read_wav_i16(OUT_DIR / "graphite_1_rm.wav")
    ch_t, tail = read_wav_i16(OUT_DIR / "stems/lock_tail.wav")
    ch_b, bed = read_wav_i16(OUT_DIR / "stems/bed.wav")
    log.ok(ch_f == 2 and len(full) // 2 == n_total, f"graphite_1.wav はステレオ・{n_total} サンプル（3,750ms）")
    log.ok(ch_r == 2 and len(rm) == len(full), "graphite_1_rm.wav は full と同じ長さ")
    log.ok(full != rm, "graphite_1_rm.wav は full と違う（風切りの層を外してある）")
    log.ok(ch_t == 2 and list(tail) == list(full[2 * tl:]),
           "lock_tail.wav が graphite_1.wav の T_lock 以降とサンプル単位で一致")
    log.ok(ch_b == 2 and len(bed) // 2 == tl, "bed.wav はステレオで、長さ = T_lock")
    lo, hi = int(doc["loop"]["bedStartMs"] * SR / 1000), int(doc["loop"]["bedEndMs"] * SR / 1000)
    cyc = (hi - lo) * 55.0 / SR
    log.ok(hi <= tl and abs(cyc - round(cyc)) < 1e-9, f"ベッドのループ長が 55Hz の整数周期（{cyc:g} 周期）で、ロックより前にある")
    peak = max(max(full), -min(full))
    log.ok(abs(db_of_peak(peak) + 6.0) <= 0.3, f"full のピーク {db_of_peak(peak):.2f} dBFS（-6 ±0.3）")
    log.ok(full[0] == 0 and full[1] == 0, "先頭のサンプルが 0")
    # クリックのステム: 長さ・尾が切れていない（最後の 1ms がほぼ 0）・先頭が 0 付近
    bad = []
    for i in range(24):
        ch, c = read_wav_i16(OUT_DIR / f"stems/click_{i:02d}.wav")
        # 尾は make.py の 4ms の終端テーパで 0 に着く（最後のサンプル = 0、最後の 1ms は全体のピークの 5% 以下）。頭は帯域フィルタの
        # 前リンギングで 0 ではない（master にも同じ形で入っている。master 側の検査は「予定された過渡」として許している）
        if ch != 1 or len(c) < 1500 or c[-1] != 0 or max(abs(v) for v in c[-44:]) > 0.05 * max(abs(v) for v in c):
            bad.append((i, ch, len(c), c[-1]))
    log.ok(not bad, f"click ステム 24 個がモノラルで、尾が 0 に着いている（切れていない）{bad if bad else ''}")
    check_events(log, doc, HEADER_PATH.read_text(encoding="utf-8"))
    # RCDATA（エディタの exe だけ）。ゲームのランタイムの app.rc には入れない
    rc = EDITOR_RC.read_text(encoding="utf-8")
    found = {m.group(1): m.group(2) for m in re.finditer(r'^\s*(UNO_CALIBRATE_\w+)\s+RCDATA\s+"([^"]+)"', rc, re.M)}
    log.ok(set(found) == set(RC_NAMES), f"editor.rc の RCDATA が {len(RC_NAMES)} 個そろっている（{len(found)} 個）")
    okp = all(found.get(k, "").endswith("assets/editor/sounds/calibrate/" + v) and
              (EDITOR_RC.parent / found[k]).resolve().is_file() for k, v in RC_FILES.items() if k in found)
    log.ok(okp, "RCDATA のパスが実在し、assets/editor/sounds/calibrate/ の正しいファイルを指す")
    log.ok("UNO_CALIBRATE" not in APP_RC.read_text(encoding="utf-8"), "app.rc（ゲームのランタイムにも付く）には入っていない")
def _mono_ok(x):
    L, R = x[:, 0], x[:, 1]
    if np.corrcoef(L, R)[0, 1] <= 0:
        return False
    W = int(0.05 * SR)
    for s in range(0, len(L) - W, W):
        a, b = L[s:s + W], R[s:s + W]
        if 10 * np.log10(np.mean(a * a + b * b) / 2 + 1e-20) > -40 and np.corrcoef(a, b)[0, 1] < -0.3:
            return False
    return True


def analyze(pcm, onsets=()):
    x = pcm.astype(np.float64) / 32768.0
    n = len(x)
    peak = np.max(np.abs(x))
    L = lufs(x.T)
    mono = x.mean(axis=1)
    dc = np.abs(x.mean(axis=0)).max()
    clip = int(np.sum(np.abs(pcm.astype(int)) >= 32767))
    step = np.max(np.abs(np.diff(x, axis=0)))
    head = np.max(np.abs(x[: int(0.001 * SR)]))
    tail50 = np.max(np.abs(x[-int(0.05 * SR):]))
    # click detector: energy above 15 kHz in 1 ms frames vs. median of a 60 ms neighbourhood
    hf = highpass(mono, 15000.0, order=4)
    fr = int(0.001 * SR)
    e = np.array([np.mean(hf[i:i + fr] ** 2) for i in range(0, n - fr, fr)]) + 1e-14
    ref_full = np.mean(mono ** 2)
    spikes = []
    for i in range(len(e)):
        nb = e[max(0, i - 30): i + 30]
        if e[i] > 100 * np.median(nb) and e[i] > ref_full * 1e-3:
            tms = i * 1.0
            if not any(-3.0 <= tms - o * 1000.0 <= 12.0 for o in onsets):
                spikes.append(tms)              # HF burst with no scheduled transient = a real click
    # centroid over time (250 ms frames)
    cents = []
    hop = int(0.25 * SR)
    win = np.hanning(4096)
    for s in range(0, n - 4096, hop):
        seg = mono[s:s + 4096] * win
        S = np.abs(np.fft.rfft(seg))
        f = _f(4096)
        en = np.sum(S)
        cents.append((s / SR, float(np.sum(f * S) / en) if en > 1e-9 else 0.0,
                      20 * np.log10(np.sqrt(np.mean(seg ** 2)) + 1e-12)))
    checks = {
        "len 2.5-4.0s": 2.5 <= n / SR <= 4.0,
        "peak -6 +-0.3": abs(20 * np.log10(peak) + 6.0) <= 0.3,
        "LUFS -20 +-1": abs(L + 20.0) <= 1.0,
        "no clip": clip == 0,
        "DC < -60dB": 20 * np.log10(dc + 1e-12) < -60,
        "first sample 0 & head<-30dB": np.all(pcm[0] == 0) and 20 * np.log10(head + 1e-12) < -30,
        "tail 50ms < -60dB": 20 * np.log10(tail50 + 1e-12) < -60,
        "max step <= 0.5*peak": step <= 0.5 * peak,
        "no unscheduled HF clicks": len(spikes) == 0,
        "mono-safe (corr>0, 50ms win corr>-0.3 above -40dB)": _mono_ok(x),
    }
    return dict(dur=n / SR, peak_db=20 * np.log10(peak), lufs=L, dc_db=20 * np.log10(dc + 1e-12),
                clip=clip, step=step / peak, spikes=spikes[:10], n_spikes=len(spikes), cents=cents,
                checks=checks, ok=all(checks.values()))




# ------------------------------------------------------------------ 検査（numpy が要る部分）

def resid_peak_dbfs(a, b):
    d = np.abs(a.astype(np.int32) - b.astype(np.int32))
    m = int(d.max()) if d.size else 0
    return -999.0 if m == 0 else db_of_peak(m)


def mix_stems(pcm_bed, click_pcm, events, tl):
    """遅い経路のロック前の混ぜ方（ランタイムと同じ: ベッド + クリックを constant-power でパン + Haas の遅延）。"""
    out = pcm_bed.astype(np.float64) / 32768.0
    for e in events:
        if e["kind"] != "click":
            continue
        s = click_pcm[e["index"]].astype(np.float64) / 32768.0
        pan = e["pan"]
        ang = (np.clip(pan, -1, 1) + 1.0) * np.pi / 4.0
        d = int(round(abs(pan) * HAAS_MS * 1e-3 * SR))
        for ch, g, dd in ((0, np.cos(ang), d if pan > 0 else 0), (1, np.sin(ang), d if pan < 0 else 0)):
            add_at(out[:, ch], e["sample"] + dd, s * g)
    return out[:tl]


def check_full(log):
    print("== calibrate resynthesis (numpy %s)" % np.__version__)
    files, inter = build_outputs()
    files2, _ = build_outputs()
    log.ok(files == files2, "2 回合成して全出力がバイト一致（決定論）")
    # コミット済みとの残差（numpy の版で最下位ビットがずれても通るよう、-90 dBFS で判定。バイト一致は参考）
    worst = -999.0
    same = 0
    for rel, data in sorted(files.items()):
        p = ROOT / rel
        if not p.is_file():
            log.ok(False, f"{rel} がコミットされていない")
            continue
        cur = p.read_bytes()
        if cur.replace(b"\r\n", b"\n") == data.replace(b"\r\n", b"\n"):
            same += 1
            continue
        if rel.endswith(".wav"):
            _, a = read_wav_i16(cur)
            _, b = read_wav_i16(data)
            if len(a) != len(b):
                log.ok(False, f"{rel} の長さが違う")
                continue
            r = resid_peak_dbfs(np.frombuffer(a, dtype="<i2"), np.frombuffer(b, dtype="<i2"))
            worst = max(worst, r)
            print(f"      {rel}: 残差 {r:.1f} dBFS（バイト一致せず）")
        else:
            print(f"      {rel}: テキストが違う（時刻の差は下で検査）")
    log.ok(worst <= -90.0, f"再合成した WAV とコミット済みの残差が -90 dBFS 以下（最大 {worst:.1f}。バイト一致 {same}/{len(files)}）")
    # 試作の master 品質の検査（peak / LUFS / DC / クリップ / 先頭と尾 / ステップ / 無予告のクリック / モノ互換）
    r = analyze(inter["pcm_full"].reshape(-1, 2), full_bus_onsets())
    for k, v in r["checks"].items():
        log.ok(v, f"full: {k}")
    log.ok(abs(r["lufs"] + 20.0) <= 1.0, f"full の LUFS {r['lufs']:.2f}")
    r2 = analyze(inter["pcm_rm"].reshape(-1, 2), full_bus_onsets())
    log.ok(r2["ok"], f"rm: master の検査が全部通る（peak {r2['peak_db']:.2f} / LUFS {r2['lufs']:.2f}）")
    # ステムからの再合成: 遅い経路のロック前だけ
    tl = inter["tl"]
    mix = mix_stems(inter["pcm_bed"], inter["click_pcm"], inter["doc"]["events"], tl)
    res = inter["pcm_full"][:tl].astype(np.float64) / 32768.0 - mix
    rms = 20 * np.log10(np.sqrt(np.mean(res ** 2)) + 1e-12)
    pk = 20 * np.log10(np.max(np.abs(res)) + 1e-12)
    ref = 20 * np.log10(np.sqrt(np.mean((inter["pcm_full"][:tl] / 32768.0) ** 2)) + 1e-12)
    log.ok(rms <= -60.0, f"ステム（ベッド + クリック）の再合成の残差（ロック前）RMS {rms:.1f} dBFS（-60 以下。ピーク {pk:.1f}、信号 RMS {ref:.1f}）")
    # lock_tail
    tail = files[(STEM_DIR / "lock_tail.wav").relative_to(ROOT).as_posix()]
    _, t = read_wav_i16(tail)
    log.ok(np.array_equal(np.frombuffer(t, dtype="<i2"), inter["pcm_full"].reshape(-1)[2 * tl:]),
           "lock_tail.wav が graphite_1.wav の T_lock 以降とサンプル単位で一致（再合成）")
    # JSON / ヘッダ（再合成した方）
    check_events(log, inter["doc"], files[HEADER_PATH.relative_to(ROOT).as_posix()].decode("utf-8"))


_FULL_ONSETS = []


def full_bus_onsets():
    return _FULL_ONSETS[0]


# ------------------------------------------------------------------ main

def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ap = argparse.ArgumentParser(description="起動音 Calibrate のデータ化（W1-BD1）")
    ap.add_argument("--check", action="store_true", help="再合成して検査（numpy が要る）。資産の検査も含む")
    ap.add_argument("--check-assets", action="store_true", help="コミット済みの資産だけの検査（標準ライブラリだけ）")
    args = ap.parse_args()

    if args.check_assets:
        log = CheckLog()
        check_assets(log)
        print("ALL", "PASS" if log.fail == 0 else f"FAIL ({log.fail})")
        return 0 if log.fail == 0 else 1

    if np is None:
        print("numpy が要ります: tools/requirements-audio.txt の版を venv に入れて実行してください。", file=sys.stderr)
        return 2

    if args.check:
        log = CheckLog()
        check_full_wrapper(log)
        check_assets(log)
        print("ALL", "PASS" if log.fail == 0 else f"FAIL ({log.fail})")
        return 0 if log.fail == 0 else 1

    files, inter = build_outputs()
    for rel, data in sorted(files.items()):
        p = ROOT / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)                           # テキストも LF 固定のバイト列のまま書く
        print(f"wrote {rel}  ({len(data)} bytes)")
    return 0


def check_full_wrapper(log):
    # analyze のクリック検出器は「予定された過渡」の時刻（Bus.onsets）を要る
    _FULL_ONSETS.clear()
    _FULL_ONSETS.append(list(synth_calibrate(True)["bus"].onsets))
    check_full(log)


if __name__ == "__main__":
    raise SystemExit(main())
