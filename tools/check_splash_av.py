#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
起動画面の A/V（音と絵）の同期を、オフラインのダンプから独立に検査する（W1-NB5。BD8 の C++ 側と対）。
窓は出さない・実音も鳴らさない。エンジンは起動せず、ダンプしたファイルだけを読む。

  python tools/check_splash_av.py <dump_dir> [--events assets/editor/sounds/calibrate/events.json]
                                  [--reference assets/editor/sounds/calibrate/graphite_1.wav] [--json out.json]

期待する時刻は `events.json`（BD1 が make.py の配置から書き出したもの）から読む。時刻・角度・クリック数を Python に書かない。
許容値は `tools/splash_av_limits.json`（av 節）。

--- ダンプの契約（BD8: `--splash-selftest --splash-dump <dir>` が書くもの） ---------------------------------
  <dir>/frames.csv   index,t,phase,percent,file,event,frame,gpu_ms,cpu_ms
                       t     … その絵の正規タイムライン上の時刻[秒]（GaugeFrame.timelineMs / 1000）
                       file  … PNG のファイル名（<dir> 相対。D2D の readback。グレースケール AA）
                       event … そのフレームで起きたイベントを `kind:index`（events.json の kind と index）で `;` 区切り。無ければ空
                       frame/gpu_ms/cpu_ms … 検査では使わない（性能の確認用）
  <dir>/av.json      {"version":1, "fps":120, "dpiScale":1.0,
                      "audio":"audio.wav", "audioOriginMs":0.0,       # audio.wav の先頭サンプルが鳴るタイムライン上の時刻
                      "layout":{"hub":[cx,cy], "needleLenPx":L, "hubRadiusPx":r,
                                "needleHalfWidthPx":w(省略可)},      # 画素の縁を原点とする連続座標（画素 (x,y) の中心は x+0.5, y+0.5）
                      "regions":{...}}                               # 黄の許可領域（splash_golden.py の region_mask 参照）
  <dir>/audio.wav    PCM16 ステレオ。ミキサーのオフライン描画（実デバイスを使わない）

注: この検査は通常（速い）経路だけが対象。Reduce Motion は針が最初からロック位置で静止し、N/S/O/L が成り立たない。
    RM の音は別に graphite_1_rm.wav を基準にして確かめる（G19）。

検査（すべて合格で終了コード 0）:
  E. 絵のイベント   : events.json の visualEvents が csv の event 列に 1 回ずつ現れ、その t が [tMs, tMs+1 フレーム) に入る
  N. 指針の角度     : 各フレームの PNG から指針の角度と先端位置を測り、正規の指針（events.json の PCHIP）と比べる
                       角度の誤差 <= needleAngleMaxErrDeg、先端の誤差 <= needleTipMaxErrPx（= 1px のずれを検出）
                       全体の時間のずれ（絵の時計 - 正規の時計）を最小二乗で推定し、|ずれ| <= skewMaxMs（= 12ms のずれを検出）
  A. 音の発音       : audio.wav から各イベントの発音（クリック・ロック・ピン）を検出し、events.json（+ audioOriginMs）と
                       audioOnsetMaxErrMs 以内。--reference があれば、全体の遅れ（相互相関）も audioReferenceMaxLagMs 以内
  S. A/V のずれ     : クリック C00..C23 の「指針がその角度に達した時刻（フレーム間を補間）」と「音の発音時刻」の差。最大 <= skewMaxMs
  L. ロックの静止   : 指針の領域がロックの静止画と一致し始めるフレームが、音のロック（latch:0）のフレームと ±lockStillMaxFrames
  O. 行き過ぎ       : C23 からロックまでの先端の接線方向の行き過ぎが overshootMaxPx（+ 先端の許容）以下。ロックで元に戻る
"""
from __future__ import annotations

import argparse
import csv
import fnmatch
import json
import math
import sys
import wave
from pathlib import Path

import numpy as np
from PIL import Image

import splash_golden as sg

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EVENTS = REPO_ROOT / "assets" / "editor" / "sounds" / "calibrate" / "events.json"
DEFAULT_REFERENCE = REPO_ROOT / "assets" / "editor" / "sounds" / "calibrate" / "graphite_1.wav"


# ---------------------------------------------------------------- 正規の指針（SplashGaugeMotion.h の PCHIP と同じ作り）


class Pchip:
    """Fritsch-Butland の単調な区分 3 次エルミート。両端の傾きは 0（現れて静止 → 着座で止まる）。"""

    def __init__(self, t, y):
        self.t = np.asarray(t, dtype=np.float64)
        self.y = np.asarray(y, dtype=np.float64)
        n = len(self.t)
        d = np.zeros(n)
        h = np.diff(self.t)
        s = np.diff(self.y) / h
        for k in range(1, n - 1):
            s0, s1 = s[k - 1], s[k]
            if s0 * s1 <= 0.0:
                d[k] = 0.0
                continue
            w1 = 2.0 * h[k] + h[k - 1]
            w2 = h[k] + 2.0 * h[k - 1]
            d[k] = (w1 + w2) / (w1 / s0 + w2 / s1)
        self.d = d

    def __call__(self, x):
        x = np.asarray(x, dtype=np.float64)
        out = np.empty_like(x)
        flat = x.reshape(-1)
        res = np.empty_like(flat)
        for i, v in enumerate(flat):
            if v <= self.t[0]:
                res[i] = self.y[0]
            elif v >= self.t[-1]:
                res[i] = self.y[-1]
            else:
                k = int(np.searchsorted(self.t, v, side="right") - 1)
                h = self.t[k + 1] - self.t[k]
                s = (v - self.t[k]) / h
                s2, s3 = s * s, s * s * s
                res[i] = ((2 * s3 - 3 * s2 + 1) * self.y[k] + (s3 - 2 * s2 + s) * h * self.d[k]
                          + (-2 * s3 + 3 * s2) * self.y[k + 1] + (s3 - s2) * h * self.d[k + 1])
        return res.reshape(out.shape)


class Events:
    def __init__(self, path: Path):
        self.raw = json.loads(Path(path).read_text(encoding="utf-8"))
        self.sr = int(self.raw["sampleRate"])
        self.events = self.raw["events"]
        self.clicks = sorted((e for e in self.events if e["kind"] == "click"), key=lambda e: e["index"])
        if len(self.clicks) < 2 or any("angleDeg" not in c for c in self.clicks):
            raise SystemExit("events.json にクリックと angleDeg がありません")
        self.lock_ms = float(self.raw["tLockMs"])
        # 指針が 0° に現れる時刻 = 最初のクリック - 最初の間隔（EXEC §2.6）
        self.needle_start_ms = 2.0 * self.clicks[0]["tMs"] - self.clicks[1]["tMs"]
        self.needle = Pchip([self.needle_start_ms] + [c["tMs"] for c in self.clicks],
                            [0.0] + [c["angleDeg"] for c in self.clicks])

    def tag(self, e) -> str:
        return f"{e['kind']}:{e['index']}"

    def select(self, patterns) -> list:
        return [e for e in self.events if any(fnmatch.fnmatch(self.tag(e), p) for p in patterns)]


# ---------------------------------------------------------------- 指針の測定（PNG から。サブピクセル）


def _accent_rb(tokens: dict) -> float:
    r, _, b = sg.hex_to_rgb(tokens["color"]["accent"]["accent"]["value"])
    return float(r - b)


def measure_needle(rgb: np.ndarray, layout: dict, accent_rb: float, band_px: float = 1.5):
    """指針の角度[度、真上 0・時計回り]と先端の位置(x,y)を返す。測れなければ None。

    重み w = (R-B)/(accent の R-B)。無彩色（地・目盛り・文字）は R≈B なので 0、黄は 1、AA の縁はその間。
    1 回目: 強い画素から軸を決める → 2 回目: 軸のそばの帯だけで重心（= 軸は必ずハブを通る）を取り直す。
    先端: 軸に沿った断面の幅の分布（線形に散らす）が、外側の平らな部分の半分になる位置。
    """
    hx, hy = layout["hub"]
    L = float(layout["needleLenPx"])
    rh = float(layout.get("hubRadiusPx", 0.0))
    half_w = float(layout.get("needleHalfWidthPx", 3.0))
    R = int(math.ceil(L + 8))
    x0, y0 = int(math.floor(hx)) - R, int(math.floor(hy)) - R
    H, W, _ = rgb.shape
    xa, ya, xb, yb = max(x0, 0), max(y0, 0), min(x0 + 2 * R, W), min(y0 + 2 * R, H)
    crop = rgb[ya:yb, xa:xb].astype(np.float64)
    w_all = np.clip((crop[..., 0] - crop[..., 2]) / accent_rb, 0.0, 1.0)
    w_all[w_all < 0.04] = 0.0
    yy, xx = np.mgrid[ya:yb, xa:xb]
    dx, dy = xx + 0.5 - hx, yy + 0.5 - hy
    r = np.hypot(dx, dy)
    r_min = max(2.5 * rh, 0.4 * L)
    ring = (r >= r_min) & (r <= L + 4)
    strong = ring & (w_all > 0.6)
    if strong.sum() < 4:
        return None

    def axis_of(wts):
        sx, sy = (wts * dx).sum(), (wts * dy).sum()
        return math.atan2(sx, -sy)       # 真上 0・時計回り（画像は y が下向き）

    theta = axis_of(strong * w_all)
    for _ in range(2):
        ux, uy = math.sin(theta), -math.cos(theta)          # 軸の向き
        s = dx * ux + dy * uy
        perp = np.abs(-dx * uy + dy * ux)
        band = ring & (perp <= half_w + band_px) & (s > 0)
        wts = np.where(band, w_all, 0.0)
        if wts.sum() < 2.0:
            return None
        theta = axis_of(wts)
    ux, uy = math.sin(theta), -math.cos(theta)
    s = dx * ux + dy * uy
    perp = np.abs(-dx * uy + dy * ux)
    sel = (perp <= half_w + band_px) & (s > 0) & (r <= L + 6) & (w_all > 0)
    ss, ww = s[sel], w_all[sel]
    if ss.size == 0:
        return None
    # 断面の幅の分布（ビン 0.25px、hat 関数で散らす）
    nb = int(math.ceil((L + 8) / 0.25)) + 8
    prof = np.zeros(nb)
    pos = ss / 0.25
    i0 = np.floor(pos).astype(int)
    fr = pos - i0
    # hat の半幅 = 1px = 4 ビン。ビンごとに距離で重み付け（面積 1 に正規化）
    for off in range(-4, 5):
        idx = i0 + off
        wgt = np.clip(1.0 - np.abs(off - fr) * 0.25, 0.0, 1.0) * 0.25
        ok = (idx >= 0) & (idx < nb)
        np.add.at(prof, idx[ok], (ww * wgt)[ok])
    plateau_lo, plateau_hi = int(0.6 * L / 0.25), int(0.85 * L / 0.25)
    plateau = float(np.median(prof[plateau_lo:plateau_hi]))
    if plateau <= 0:
        return None
    half = 0.5 * plateau
    start = min(int(0.9 * L / 0.25), nb - 2)
    k = start
    while k < nb - 1 and prof[k] >= half:
        k += 1
    if k >= nb - 1:
        return None
    a, b = prof[k - 1], prof[k]
    f = (a - half) / (a - b) if a != b else 0.0
    tip_s = (k - 1 + f + 0.5) * 0.25 - 0.125     # ビン中心 → 位置
    tip = (hx + ux * tip_s, hy + uy * tip_s)
    return math.degrees(theta) % 360.0, tip


# ---------------------------------------------------------------- 音


def read_wav(path: Path):
    with wave.open(str(path), "rb") as w:
        n, ch, sw, sr = w.getnframes(), w.getnchannels(), w.getsampwidth(), w.getframerate()
        if sw != 2:
            raise SystemExit(f"{path}: PCM16 だけ対応（{sw * 8}bit）")
        x = np.frombuffer(w.readframes(n), dtype="<i2").reshape(-1, ch).astype(np.float64) / 32768.0
    return x.mean(axis=1), sr


def detect_onset(mono: np.ndarray, sr: int, expected_sample: float, pre_ms=4.0, post_ms=12.0, frac=0.2):
    """期待位置の近くで、2 階差（高域）の包絡が窓の最大の frac を最初に超えるサンプル。見つからなければ None。"""
    e = int(round(expected_sample))
    pre, post = int(pre_ms * sr / 1000.0), int(post_ms * sr / 1000.0)
    lo, hi = max(e - pre, 2), min(e + post, len(mono))
    if hi - lo < 8:
        return None
    env = np.abs(np.diff(mono[lo - 2:hi], 2))
    pk = env.max()
    if pk <= 1e-6:
        return None
    return lo + int(np.argmax(env > pk * frac))


def xcorr_lag(ref: np.ndarray, sig: np.ndarray, max_lag: int) -> int:
    """sig が ref より何サンプル遅れているか（正 = 遅い）。先頭 2 秒相当の FFT 相互相関。"""
    n = 1 << int(math.ceil(math.log2(len(ref) + len(sig) + 1)))
    a, b = np.fft.rfft(ref, n), np.fft.rfft(sig, n)
    c = np.fft.irfft(b * np.conj(a), n)
    lags = np.concatenate([np.arange(0, max_lag + 1), np.arange(-max_lag, 0)])
    vals = np.concatenate([c[0:max_lag + 1], c[n - max_lag:n]])
    return int(lags[int(np.argmax(vals))])


# ---------------------------------------------------------------- 本体


class Report:
    def __init__(self):
        self.items = []

    def add(self, key: str, ok: bool, msg: str, **data):
        self.items.append({"check": key, "ok": bool(ok), "msg": msg, **data})
        print(f"[{'OK' if ok else 'NG'}] {key}: {msg}")

    @property
    def ok(self):
        return all(i["ok"] for i in self.items)


def _parabola_min(xs, ys, i):
    if 0 < i < len(xs) - 1:
        a, b, c = ys[i - 1], ys[i], ys[i + 1]
        den = a - 2 * b + c
        if den > 0:
            return xs[i] + 0.5 * (a - c) / den * (xs[1] - xs[0])
    return xs[i]


def run(dump: Path, events_path: Path, limits_path: Path, tokens_path: Path, reference: Path | None) -> Report:
    rep = Report()
    lim = json.loads(Path(limits_path).read_text(encoding="utf-8"))["av"]
    tokens = json.loads(Path(tokens_path).read_text(encoding="utf-8"))
    ev = Events(events_path)
    av = json.loads((dump / "av.json").read_text(encoding="utf-8"))
    layout = av["layout"]
    with open(dump / "frames.csv", encoding="utf-8", newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        rep.add("frames", False, "frames.csv が空です")
        return rep
    rows.sort(key=lambda r: float(r["t"]))
    ts = np.array([float(r["t"]) * 1000.0 for r in rows])
    dt = float(np.median(np.diff(ts))) if len(ts) > 1 else 1000.0 / float(av.get("fps", 60))
    rep.add("frames", True, f"{len(rows)} フレーム、間隔 {dt:.3f}ms ({1000.0 / dt:.1f}fps)")

    # --- E. 絵のイベント
    tags: dict[str, list[float]] = {}
    for r, t in zip(rows, ts):
        for tag in filter(None, (s.strip() for s in (r.get("event") or "").split(";"))):
            tags.setdefault(tag, []).append(float(t))
    bad = []
    worst = 0.0
    for e in ev.select(lim["visualEvents"]):
        tag = ev.tag(e)
        got = tags.get(tag, [])
        if len(got) != 1:
            bad.append(f"{tag}: {len(got)} 回")
            continue
        d = (got[0] - e["tMs"]) / dt
        worst = max(worst, abs(d))
        if d < -1e-6 or d >= lim["visualEventMaxFrames"] + 1e-6:
            bad.append(f"{tag}: {d:+.2f} フレーム（{got[0] - e['tMs']:+.2f}ms）")
    rep.add("E.visual_events", not bad, "全イベントが期待のフレームにある" if not bad else "; ".join(bad[:6]) + (" ..." if len(bad) > 6 else ""),
            worstFrames=worst)

    # --- N. 指針
    accent_rb = _accent_rb(tokens)
    L = float(layout["needleLenPx"])
    last_click = ev.clicks[-1]["tMs"]
    t0 = ev.needle_start_ms + lim["measureWindowMs"]["firstPad"]
    meas = []     # (t, theta, tip)
    for r, t in zip(rows, ts):
        if not (t0 <= t <= last_click):
            continue
        rgb = sg.load_rgb(dump / r["file"])
        m = measure_needle(rgb, layout, accent_rb, lim["measureWindowMs"]["axisBandPx"])
        if m is not None:
            meas.append((t, m[0], m[1]))
    if len(meas) < 10:
        rep.add("N.needle", False, f"指針を測れたフレームが {len(meas)} 枚しかありません（黄の指針が見えない?）")
        return rep
    mt = np.array([m[0] for m in meas])
    exp = ev.needle(mt)
    th = np.array([m[1] for m in meas])
    th = exp + ((th - exp + 180.0) % 360.0 - 180.0)         # 360° の継ぎ目をたたむ
    err = th - exp
    tip_exp = np.array([(layout["hub"][0] + L * math.sin(math.radians(e)), layout["hub"][1] - L * math.cos(math.radians(e))) for e in exp])
    tip_got = np.array([m[2] for m in meas])
    tip_err = np.hypot(*(tip_got - tip_exp).T)
    rep.add("N.needle_angle", float(np.abs(err).max()) <= lim["needleAngleMaxErrDeg"],
            f"角度の誤差 最大 {np.abs(err).max():.3f}° / 平均 {np.abs(err).mean():.3f}°（上限 {lim['needleAngleMaxErrDeg']}°、{len(meas)} 枚）",
            maxErrDeg=float(np.abs(err).max()))
    rep.add("N.needle_tip", float(tip_err.max()) <= lim["needleTipMaxErrPx"],
            f"先端の誤差 最大 {tip_err.max():.3f}px / 平均 {tip_err.mean():.3f}px（上限 {lim['needleTipMaxErrPx']}px）",
            maxErrPx=float(tip_err.max()))
    offs = np.arange(-40.0, 40.0001, 0.25)
    rms = np.array([float(np.sqrt(np.mean((th - ev.needle(mt + o)) ** 2))) for o in offs])
    i = int(np.argmin(rms))
    off = _parabola_min(offs, rms, i)
    rep.add("N.clock_skew", abs(off) <= lim["skewMaxMs"],
            f"絵の時計の推定ずれ {off:+.2f}ms（上限 ±{lim['skewMaxMs']}ms。正 = 絵が正規より進んでいる）", skewMs=float(off))

    # --- A. 音
    origin = float(av.get("audioOriginMs", 0.0))
    audio_path = dump / av.get("audio", "audio.wav")
    mono, sr = read_wav(audio_path)
    if sr != ev.sr:
        rep.add("A.audio", False, f"サンプルレート {sr} が events.json の {ev.sr} と違います")
        return rep
    onsets = {}
    worst_a, bad = 0.0, []
    for e in ev.select(lim["audioOnsetEvents"]):
        exp_s = (e["tMs"] - origin) * sr / 1000.0
        got = detect_onset(mono, sr, exp_s)
        tag = ev.tag(e)
        if got is None:
            bad.append(f"{tag}: 発音なし")
            continue
        e_ms = (got - exp_s) * 1000.0 / sr
        onsets[tag] = (got * 1000.0 / sr) + origin
        worst_a = max(worst_a, abs(e_ms))
        if abs(e_ms) > lim["audioOnsetMaxErrMs"]:
            bad.append(f"{tag}: {e_ms:+.2f}ms")
    rep.add("A.audio_onsets", not bad, f"最大 {worst_a:.3f}ms（上限 {lim['audioOnsetMaxErrMs']}ms）" if not bad else "; ".join(bad[:6]),
            worstMs=worst_a)
    if reference is not None:
        rmono, rsr = read_wav(reference)
        n = int(2.0 * sr)
        shift = int(round(origin * sr / 1000.0))
        seg = mono[:n]
        ref_seg = rmono[max(shift, 0):max(shift, 0) + n]
        lag = xcorr_lag(ref_seg, seg, int(0.1 * sr))
        lag_ms = lag * 1000.0 / sr
        rep.add("A.audio_reference_lag", abs(lag_ms) <= lim["audioReferenceMaxLagMs"],
                f"基準の WAV との遅れ {lag_ms:+.3f}ms（上限 ±{lim['audioReferenceMaxLagMs']}ms）", lagMs=lag_ms)

    # --- S. A/V のずれ（指針が角度に達した時刻と音）
    skews = []
    for c in ev.clicks:
        tag = ev.tag(c)
        if tag not in onsets:
            continue
        target = float(c["angleDeg"])
        k = int(np.argmax(th >= target)) if (th >= target).any() else -1
        if k <= 0:
            continue
        t_vis = mt[k - 1] + (target - th[k - 1]) / (th[k] - th[k - 1]) * (mt[k] - mt[k - 1])
        skews.append((tag, float(t_vis - onsets[tag])))
    if skews:
        sk = np.array([s for _, s in skews])
        rep.add("S.av_skew", float(np.abs(sk).max()) <= lim["skewMaxMs"],
                f"指針の到達と音の差 最大 {np.abs(sk).max():+.2f}ms / 中央値 {np.median(sk):+.2f}ms（{len(sk)} 点、上限 ±{lim['skewMaxMs']}ms）",
                maxMs=float(np.abs(sk).max()), medianMs=float(np.median(sk)))
    else:
        rep.add("S.av_skew", False, "指針の到達時刻を求められるクリックがありません")

    # --- L / O. ロックの静止と行き過ぎ
    lock_tag = lim["lockEvent"]
    lock_ev = next((e for e in ev.events if ev.tag(e) == lock_tag), None)
    roi_r = float(layout.get("needleRoiRadiusPx", L))
    hx, hy = layout["hub"]
    ping_ms = next(e["tMs"] for e in ev.events if e["kind"] == "ping")
    idx_in = [i for i in range(len(rows)) if ev.clicks[-1]["tMs"] <= ts[i] <= ping_ms]
    if lock_ev and len(idx_in) >= 3:
        # 静止 = 針だけの先端がほぼ動かない（ロックの後も目盛りの波・ベゼルの光・三角などは動き続けるので、
        # 画面や ROI の全体比較はしない。針の黄のマスクだけで測る）
        tip_tol = float(lim.get("lockStillTipPx", 0.3))
        tips_in = {}
        for i in idx_in:
            m = measure_needle(sg.load_rgb(dump / rows[i]["file"]), layout, accent_rb, lim["measureWindowMs"]["axisBandPx"])
            if m:
                tips_in[i] = np.array(m[1])
        last = idx_in[-1]
        if last in tips_in:
            first_same = last
            for i in reversed(idx_in):
                if i in tips_in and float(np.hypot(*(tips_in[i] - tips_in[last]))) <= tip_tol:
                    first_same = i
                else:
                    break
        else:
            first_same = None
        audio_lock_ms = onsets.get(lock_tag, lock_ev["tMs"])
        audio_frame = int(np.argmax(ts >= audio_lock_ms - 1e-6)) if (ts >= audio_lock_ms - 1e-6).any() else len(ts)
        if first_same is None:
            rep.add("L.lock_still", False, "最後のフレームで針を測れません")
        else:
            d = first_same - audio_frame
            rep.add("L.lock_still", abs(d) <= lim["lockStillMaxFrames"],
                    f"針の先端の静止が始まるフレームと音のロックの差 {d:+d} フレーム（上限 ±{lim['lockStillMaxFrames']}、先端の許容 {tip_tol}px）", frames=d)
    else:
        rep.add("L.lock_still", False, "ロックの区間のフレームが足りません")
    # 行き過ぎ: C23 からロックまでの先端の、最終の先端からの接線方向のずれ
    ov_idx = [i for i in range(len(rows)) if ev.clicks[-1]["tMs"] - 1e-6 <= ts[i] <= ev.lock_ms + dt]
    tips = []
    for i in ov_idx:
        m = measure_needle(sg.load_rgb(dump / rows[i]["file"]), layout, accent_rb, lim["measureWindowMs"]["axisBandPx"])
        if m:
            tips.append((ts[i], m[1]))
    if len(tips) >= 3:
        final_tip = np.array(tips[-1][1])
        # 先端の接線方向（時計回り）= 最終の針の向きに直交。真上 0・時計回りの角 θ なら (cosθ, sinθ)
        fx, fy = final_tip[0] - hx, final_tip[1] - hy
        nrm = math.hypot(fx, fy) or 1.0
        tx, ty = -fy / nrm, fx / nrm
        dxs = np.array([(t[1][0] - final_tip[0]) * tx + (t[1][1] - final_tip[1]) * ty for t in tips])
        peak = float(dxs.max())
        limit = lim["overshootMaxPx"] + lim["needleTipMaxErrPx"]
        floor = float(lim.get("overshootMinPx", 1.0))
        rep.add("O.overshoot", floor <= peak <= limit,
                f"行き過ぎのピーク {peak:.2f}px（下限 {floor}px、上限 {lim['overshootMaxPx']}px + 測定の許容 {lim['needleTipMaxErrPx']}px）", peakPx=peak)
    else:
        rep.add("O.overshoot", False, "行き過ぎを測れるフレームが足りません")
    return rep


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path)
    ap.add_argument("--events", type=Path, default=DEFAULT_EVENTS)
    ap.add_argument("--reference", type=Path, default=None, help="基準の WAV（graphite_1.wav / graphite_1_rm.wav）。速い経路の全体の遅れを測る")
    ap.add_argument("--limits", type=Path, default=sg.DEFAULT_LIMITS)
    ap.add_argument("--tokens", type=Path, default=sg.DEFAULT_TOKENS)
    ap.add_argument("--json", type=Path)
    a = ap.parse_args(argv)
    rep = run(a.dump, a.events, a.limits, a.tokens, a.reference)
    if a.json:
        a.json.write_text(json.dumps({"ok": rep.ok, "checks": rep.items}, ensure_ascii=False, indent=1), encoding="utf-8")
    print("結果:", "すべて合格" if rep.ok else "不合格")
    return 0 if rep.ok else 1


if __name__ == "__main__":
    sys.exit(main())
