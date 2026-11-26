#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_splash_av.py / splash_golden.py / splash_av_preview.py の単体テスト（W1-NB5）。ctest の SplashAvPyTests から呼ばれる。
窓は出さない・エンジンも起動しない。起動画面の絵は、トークンの色だけで描いた合成のダンプ（指針の PCHIP は events.json から）。

  python tools/test_splash_av.py       （numpy と Pillow が要る。pytest は要らない）
"""
from __future__ import annotations

import csv
import json
import math
import shutil
import sys
import tempfile
import unittest
import wave
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_splash_av as av   # noqa: E402
import splash_golden as sg     # noqa: E402

REPO = Path(__file__).resolve().parent.parent
EVENTS = REPO / "assets/editor/sounds/calibrate/events.json"
MASTER = REPO / "assets/editor/sounds/calibrate/graphite_1.wav"
TOK = json.loads(sg.DEFAULT_TOKENS.read_text(encoding="utf-8"))
BG = np.array(sg.hex_to_rgb(TOK["color"]["surface"]["bg0"]["value"]), dtype=np.float64)
ACCENT = np.array(sg.hex_to_rgb(TOK["color"]["accent"]["accent"]["value"]), dtype=np.float64)
W, H = 640, 400
HUB = (320.0, 230.0)
NEEDLE_LEN, HUB_R = 150.0, 8.0


def needle_coverage(hub, theta_deg, length, shape, w_hub=4.4, w_tip=2.4, ss=4):
    """指針（台形）の被覆率 0..1。画素ごとに ss×ss のサンプル。針の外接矩形だけを計算する。"""
    h, w = shape
    th = math.radians(theta_deg)
    ux, uy = math.sin(th), -math.cos(th)
    ex, ey = hub[0] + ux * length, hub[1] + uy * length
    pad = 4
    x0, x1 = int(min(hub[0], ex)) - pad, int(max(hub[0], ex)) + pad
    y0, y1 = int(min(hub[1], ey)) - pad, int(max(hub[1], ey)) + pad
    x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, w), min(y1, h)
    off = (np.arange(ss) + 0.5) / ss
    ys = (np.arange(y0, y1)[:, None] + off[None, :]).reshape(-1)
    xs = (np.arange(x0, x1)[:, None] + off[None, :]).reshape(-1)
    dx = xs[None, :] - hub[0]
    dy = ys[:, None] - hub[1]
    s = dx * ux + dy * uy
    perp = np.abs(-dx * uy + dy * ux)
    half = 0.5 * (w_hub + (w_tip - w_hub) * np.clip(s / length, 0, 1))
    inside = ((s >= 0) & (s <= length) & (perp <= half)).astype(np.float32)
    cov = inside.reshape(y1 - y0, ss, x1 - x0, ss).mean(axis=(1, 3)).astype(np.float64)
    return x0, y0, cov


def over(img, x0, y0, cov, color, alpha=1.0):
    a = (cov * alpha)[..., None]
    reg = img[y0:y0 + cov.shape[0], x0:x0 + cov.shape[1]]
    reg[:] = reg * (1 - a) + color[None, None, :] * a


def render_frame(theta, dx_px=0.0, extra_len=0.0, anim=0.0):
    img = np.empty((H, W, 3), dtype=np.float64)
    img[:] = BG
    if anim:      # ロックの後の目盛りの波・ベゼルの光（針より下の層、無彩色）
        yy, xx = np.mgrid[0:H, 0:W]
        rr = np.hypot(xx + 0.5 - HUB[0], yy + 0.5 - HUB[1])
        gray = np.array([200.0, 200.0, 200.0])
        for r0 in (NEEDLE_LEN + 4, NEEDLE_LEN - 6):
            band = np.clip(1.5 - np.abs(rr - r0 - 6.0 * anim), 0, 1)[..., None] * 0.6
            img[:] = img * (1 - band) + gray * band
    hub = (HUB[0] + dx_px, HUB[1])
    # 残像線（1px、α 24% / 10%、後ろ 2.5° / 5°）。面ではなく線
    for back, al in ((2.5, 0.24), (5.0, 0.10)):
        x0, y0, cov = needle_coverage(hub, theta - back, NEEDLE_LEN * 0.9, img.shape[:2], 1.0, 1.0)
        over(img, x0, y0, cov, ACCENT, al)
    x0, y0, cov = needle_coverage(hub, theta, NEEDLE_LEN + extra_len, img.shape[:2])
    over(img, x0, y0, cov, ACCENT)
    hx0, hy0 = int(HUB[0]) - 16, int(HUB[1]) - 16
    yy, xx = np.mgrid[hy0:hy0 + 32, hx0:hx0 + 32]
    disc = np.clip(HUB_R + 0.5 - np.hypot(xx + 0.5 - HUB[0], yy + 0.5 - HUB[1]), 0, 1)
    over(img, hx0, hy0, disc, ACCENT)
    return img


def write_png(path, img):
    Image.fromarray(np.clip(np.rint(img), 0, 255).astype(np.uint8), "RGB").save(path)


def make_dump(root: Path, fps=120, render_time_shift_ms=0.0, needle_dx_px=0.0, needle_extra_len=0.0,
              audio_shift_ms=0.0, label_shift_ms=0.0, post_lock_anim=False, late_lock_ms=0.0, no_overshoot=False):
    """合成のダンプ（BD8 の契約どおり）。引数でわざとずらして、検査が見つけることを確かめる。"""
    ev = av.Events(EVENTS)
    dt = 1000.0 / fps
    t_start, t_end = 160.0, 2200.0
    n = int((t_end - t_start) / dt)
    root.mkdir(parents=True, exist_ok=True)
    rows = []
    labels = {}
    for e in ev.select(["click:*", "latch:*", "bell:*", "ping:0"]):
        k = int(math.ceil((e["tMs"] + label_shift_ms - t_start) / dt - 1e-9))
        labels.setdefault(k, []).append(ev.tag(e))
    last_click = ev.clicks[-1]["tMs"]
    span = ev.lock_ms - last_click
    cache = {}
    for k in range(n):
        t = t_start + k * dt
        te = t + render_time_shift_ms
        theta = float(ev.needle(np.array([te]))[0])
        ov = 0.0
        if last_click < te < ev.lock_ms:
            s = (te - last_click) / span
            ov = 2.0 * 16.0 * (s * (1 - s)) ** 2        # 行き過ぎ px（SplashGaugeMotion.h と同じ式）
        if no_overshoot:
            ov = 0.0
        if late_lock_ms and ev.lock_ms <= te < ev.lock_ms + late_lock_ms:
            ov += 3.0 * (1.0 - (te - ev.lock_ms) / late_lock_ms)       # ロックの後も針が戻り続ける（視覚のロックが遅い）
        anim = 0.0
        if post_lock_anim and ev.lock_ms <= te <= ev.lock_ms + 220.0:
            anim = (te - ev.lock_ms) / 220.0                              # ロックの後の目盛りの波・ベゼルの光（針は静止）
        key = (round(theta, 6), round(ov, 6), round(anim, 6))
        if key not in cache:
            # 先端の接線方向へ ov px = 針を ov/L ラジアン余計に回す
            fr = render_frame(theta + math.degrees(ov / NEEDLE_LEN), dx_px=needle_dx_px, extra_len=needle_extra_len, anim=anim)
            cache[key] = fr
        name = f"f{k:04d}.png"
        write_png(root / name, cache[key])
        rows.append({"index": k, "t": f"{t / 1000.0:.6f}", "phase": "boot", "percent": "0", "file": name,
                     "event": ";".join(labels.get(k, [])), "frame": k, "gpu_ms": "0", "cpu_ms": "0"})
    with open(root / "frames.csv", "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    # 音: graphite_1.wav を audio_shift_ms だけ遅らせる（正）/ 進める（負）
    with wave.open(str(MASTER), "rb") as src:
        sr, ch, nfr = src.getframerate(), src.getnchannels(), src.getnframes()
        pcm = np.frombuffer(src.readframes(nfr), dtype="<i2").reshape(-1, ch)
    sh = int(round(audio_shift_ms * sr / 1000.0))
    if sh > 0:
        pcm = np.concatenate([np.zeros((sh, ch), dtype="<i2"), pcm])
    elif sh < 0:
        pcm = pcm[-sh:]
    with wave.open(str(root / "audio.wav"), "wb") as out:
        out.setnchannels(ch)
        out.setsampwidth(2)
        out.setframerate(sr)
        out.writeframes(pcm.astype("<i2").tobytes())
    (root / "av.json").write_text(json.dumps({
        "version": 1, "fps": fps, "dpiScale": 1.0, "audio": "audio.wav", "audioOriginMs": 0.0,
        "layout": {"hub": list(HUB), "needleLenPx": NEEDLE_LEN, "hubRadiusPx": HUB_R, "needleHalfWidthPx": 2.2},
        "regions": {}}), encoding="utf-8")
    return root


class AvCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="splash_av_test_"))
        cls.cache = {}

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_case(self, name, **kw):
        if name not in self.cache:
            d = make_dump(self.tmp / name, **kw)
            self.cache[name] = av.run(d, EVENTS, sg.DEFAULT_LIMITS, sg.DEFAULT_TOKENS, MASTER)
        return self.cache[name]

    @staticmethod
    def failed(rep):
        return {i["check"] for i in rep.items if not i["ok"]}

    def test_clean_dump_passes_all(self):
        rep = self.run_case("clean")
        self.assertEqual(self.failed(rep), set(), [i for i in rep.items if not i["ok"]])

    def test_lock_still_ignores_post_lock_tick_and_bezel_animation(self):
        rep = self.run_case("postlock", post_lock_anim=True)
        self.assertEqual(self.failed(rep), set(), [i for i in rep.items if not i["ok"]])

    def test_detects_late_visual_lock(self):
        rep = self.run_case("latelock", late_lock_ms=60.0)
        self.assertIn("L.lock_still", self.failed(rep))

    def test_detects_missing_overshoot(self):
        rep = self.run_case("noov", no_overshoot=True)
        self.assertIn("O.overshoot", self.failed(rep))

    def test_detects_12ms_skew_in_picture(self):
        rep = self.run_case("t12", render_time_shift_ms=12.0)
        f = self.failed(rep)
        self.assertIn("N.clock_skew", f)
        self.assertIn("S.av_skew", f)

    def test_detects_12ms_skew_in_audio(self):
        rep = self.run_case("a12", audio_shift_ms=12.0)
        f = self.failed(rep)
        self.assertIn("A.audio_onsets", f)
        self.assertIn("A.audio_reference_lag", f)
        self.assertIn("S.av_skew", f)

    def test_detects_12ms_skew_in_event_labels(self):
        rep = self.run_case("l12", label_shift_ms=12.0)
        self.assertIn("E.visual_events", self.failed(rep))

    def test_detects_1px_sideways_shift(self):
        rep = self.run_case("px1", needle_dx_px=1.0)
        self.assertIn("N.needle_tip", self.failed(rep))

    def test_detects_1px_longer_needle(self):
        rep = self.run_case("len1", needle_extra_len=1.0)
        self.assertIn("N.needle_tip", self.failed(rep))


class MeasureTests(unittest.TestCase):
    def test_measure_accuracy_over_angles(self):
        layout = {"hub": list(HUB), "needleLenPx": NEEDLE_LEN, "hubRadiusPx": HUB_R, "needleHalfWidthPx": 2.2}
        rb = av._accent_rb(TOK)
        worst_a = worst_t = 0.0
        for th in (3.0, 15.0, 44.7, 90.0, 133.3, 181.0, 270.5, 359.0):
            img = np.rint(render_frame(th)).astype(np.uint8)
            m = av.measure_needle(img, layout, rb)
            self.assertIsNotNone(m)
            d = (m[0] - th + 180) % 360 - 180
            tip = (HUB[0] + NEEDLE_LEN * math.sin(math.radians(th)), HUB[1] - NEEDLE_LEN * math.cos(math.radians(th)))
            worst_a = max(worst_a, abs(d))
            worst_t = max(worst_t, math.hypot(m[1][0] - tip[0], m[1][1] - tip[1]))
        self.assertLess(worst_a, 0.2)
        self.assertLess(worst_t, 0.4)


class PchipTests(unittest.TestCase):
    def test_needle_passes_click_points_and_is_monotone(self):
        ev = av.Events(EVENTS)
        for c in ev.clicks:
            self.assertAlmostEqual(float(ev.needle(np.array([c["tMs"]]))[0]), c["angleDeg"], places=6)
        ts = np.linspace(ev.needle_start_ms, ev.clicks[-1]["tMs"], 2000)
        self.assertTrue(np.all(np.diff(ev.needle(ts)) >= -1e-9))
        self.assertAlmostEqual(ev.needle_start_ms, 177.2, delta=0.2)    # EXEC §2.6: 268 - 91 = 177ms

    def test_events_json_matches_exec_table(self):
        ev = av.Events(EVENTS)
        self.assertEqual(len(ev.clicks), 24)
        self.assertAlmostEqual(ev.lock_ms, 1628.0, delta=0.5)
        self.assertAlmostEqual(ev.clicks[0]["tMs"], 268.0, delta=0.5)
        self.assertAlmostEqual(ev.clicks[23]["tMs"], 1543.0, delta=0.6)


def synth_boot_frame(extra=None):
    """トークンの色だけの起動画面っぽい絵（地・方眼・指針・ハブ・四隅のトンボ）。"""
    img = np.empty((540, 960, 3), dtype=np.float64)
    img[:] = BG
    grid = np.array(sg.hex_to_rgb(TOK["color"]["viewport"]["gridMinor"]["value"]), dtype=np.float64)
    for x in range(0, 960, 40):
        img[:, x] = 0.55 * grid + 0.45 * BG
    x0, y0, cov = needle_coverage((480.0, 300.0), 200.0, 120.0, img.shape[:2])
    over(img, x0, y0, cov, ACCENT)
    yy, xx = np.mgrid[0:540, 0:960]
    over(img, 0, 0, np.clip(4.0 - np.hypot(xx + 0.5 - 480.0, yy + 0.5 - 300.0), 0, 1), ACCENT)
    for cx, cy in ((28, 28), (908, 28), (28, 488), (908, 488)):      # トンボ: 腕 24 / 太さ 2
        img[cy:cy + 2, cx:cx + 24] = ACCENT
        img[cy:cy + 24, cx:cx + 2] = ACCENT
    if extra:
        extra(img)
    return np.clip(np.rint(img), 0, 255).astype(np.uint8)


BOOT_REGIONS = {"tombo": [[28, 28, 52, 52], [908, 28, 932, 52], [28, 488, 52, 512], [908, 488, 932, 512]],
                "needle": [{"circle": [480.0, 300.0, 125.0]}], "hub": [{"circle": [480.0, 300.0, 5.0]}]}


class GoldenTests(unittest.TestCase):
    pol = None

    @classmethod
    def setUpClass(cls):
        cls.pol = sg.load_policy()

    def test_policy_is_read_from_tokens(self):
        p = self.pol
        self.assertEqual((p.edit_ratio_pct, p.play_ratio_pct, p.edit_max_lines), (1.5, 2.5, 9))
        self.assertEqual(set(p.allowed_boot), {"tombo", "lockTriangle", "needle", "hub", "stepOverline"})
        self.assertEqual(p.tombo_arms_px, [10, 9])
        self.assertGreater(len(p.token_rgbs), 40)

    def test_accent_is_inside_yellow_zone(self):
        _, c, h = sg.oklch_arrays(ACCENT[None, :])
        self.assertGreaterEqual(c[0], self.pol.chroma_min)
        self.assertTrue(self.pol.hue_min <= h[0] <= self.pol.hue_max)

    def test_clean_boot_frame_passes(self):
        r = sg.check_yellow(synth_boot_frame(), "boot", self.pol, BOOT_REGIONS)
        self.assertTrue(r.ok, r.failures)
        self.assertEqual(r.face_px, 0)

    def test_yellow_face_is_detected(self):
        def glow(img):
            img[200:240, 100:140] = ACCENT * 0.8 + BG * 0.2
        r = sg.check_yellow(synth_boot_frame(glow), "boot", self.pol, BOOT_REGIONS)
        self.assertGreater(r.face_px, 0)
        self.assertFalse(r.ok)

    def test_yellow_outside_allowed_regions_is_detected(self):
        def line(img):
            img[100:102, 300:420] = ACCENT         # 許可していない場所の黄の線
        r = sg.check_yellow(synth_boot_frame(line), "boot", self.pol, BOOT_REGIONS)
        self.assertGreater(r.outside_region_px, 0)
        self.assertFalse(r.ok)

    def test_off_token_color_is_detected(self):
        def bad(img):
            img[300:310, 100:110] = (255, 0, 255)
        r = sg.check_yellow(synth_boot_frame(bad), "boot", self.pol, BOOT_REGIONS)
        self.assertGreater(r.off_token_px, 0)
        self.assertFalse(r.ok)

    def test_aa_blend_of_two_tokens_is_not_off_token(self):
        n, _ = sg.off_token_pixels(synth_boot_frame()[..., :3], self.pol.token_rgbs, self.pol.aa_tolerance)
        self.assertEqual(n, 0)

    def test_ratio_limit_and_face_in_edit_mode(self):
        def band(img):
            img[100:140, :] = ACCENT                 # 画面の 7% を黄で塗る
        r = sg.check_yellow(synth_boot_frame(band), "edit", self.pol)
        self.assertTrue(any("画素比" in f for f in r.failures))
        self.assertTrue(any("面" in f for f in r.failures))

    def test_edit_mode_counts_lines_and_tombo_as_one_set(self):
        img = np.empty((600, 900, 3), dtype=np.uint8)
        img[:] = BG.astype(np.uint8)
        a = ACCENT.astype(np.uint8)
        for cx, cy in ((100, 100), (200, 100), (100, 200), (200, 200)):       # 10px のトンボ 1 組 = 4 隅
            img[cy:cy + 2, cx:cx + 10] = a
            img[cy:cy + 10, cx:cx + 2] = a
        for i in range(8):                                                      # 長い線 8 本
            img[300 + i * 20:302 + i * 20, 50:400] = a
        r = sg.check_yellow(img, "edit", self.pol)
        self.assertEqual(r.lines, 9)
        self.assertEqual(r.face_px, 0)
        self.assertTrue(r.ok, r.failures)
        img[10:12, 600:800] = a                                                 # 10 本目
        self.assertFalse(sg.check_yellow(img, "edit", self.pol).ok)

    def test_edit_viewport_excluded_from_all_checks(self):
        """実エディタの画面: ビューポートの描画（トークン外の色・黄っぽい砂・焚き火）を exclude で外せる。"""
        img = np.empty((600, 900, 3), dtype=np.uint8)
        img[:] = BG.astype(np.uint8)
        rng = np.random.default_rng(1)
        vp = [300, 100, 800, 500]
        img[100:500, 300:800] = rng.integers(0, 256, (400, 500, 3), dtype=np.uint8)   # トークン外の雑多な色
        img[200:300, 400:500] = (230, 190, 40)                                         # 黄の面（焚き火）
        r = sg.check_yellow(img, "edit", self.pol)
        self.assertFalse(r.ok)
        r = sg.check_yellow(img, "edit", self.pol, exclude=[vp])
        self.assertTrue(r.ok, r.failures)
        self.assertEqual((r.off_token_px, r.face_px), (0, 0))
        # 外した領域の外の違反は引き続き検出する
        img[20:60, 20:60] = (255, 0, 255)
        self.assertFalse(sg.check_yellow(img, "edit", self.pol, exclude=[vp]).ok)

    def test_ssim(self):
        a = synth_boot_frame()
        self.assertAlmostEqual(sg.ssim_gray(a, a), 1.0, places=6)
        b = np.roll(a, 3, axis=1)
        self.assertLess(sg.ssim_gray(a, b), 0.95)


class PreviewTests(unittest.TestCase):
    def test_ffmpeg_command_is_built_without_running(self):
        import splash_av_preview as pv
        tmp = Path(tempfile.mkdtemp(prefix="splash_pv_test_"))
        try:
            rows = [{"t": "0.000", "file": "a.png"}, {"t": "0.020", "file": "b.png"}, {"t": "0.050", "file": "c.png"}]
            n = pv.write_concat_list(tmp / "list.txt", tmp, rows, tail_s=0.1)
            txt = (tmp / "list.txt").read_text(encoding="utf-8")
            self.assertIn("duration 0.020000", txt)
            self.assertIn("duration 0.030000", txt)
            self.assertEqual(n, 3)
            cmd = pv.build_command("ffmpeg", tmp / "list.txt", tmp / "a.wav", tmp / "o.mp4", audio_origin_ms=0.0, first_t_ms=0.0)
            self.assertIn("libx264", cmd)
            self.assertIn("-shortest", cmd)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
