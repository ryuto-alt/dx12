#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Graphite Gauge の「黄の予算」検査と、起動画面のキーフレーム比較（W1-NB5）。

GG の `brand/verify.py`（v3.1）の移植＋一般化。窓は出さない（PNG を読むだけ。エンジンは起動しない）。

  python tools/splash_golden.py yellow <png...> --mode boot|edit|play [--regions regions.json] [--scale 1.5]
  python tools/splash_golden.py dir <dump_dir>   --mode boot [--scale ...]      # <dir> の *.png を全部検査（regions は <dir>/av.json から）
  python tools/splash_golden.py ssim <actual.png> <reference.png> [--min 0.85]
  python tools/splash_golden.py policy                                          # tokens から読んだ予算を表示

方針（EXEC §2.7 NB5、§0 の「期待値を手で書かない」）:
  - 黄の範囲・黄を許す要素の集合・画素比の上限（編集 / 再生）・線の本数・トンボの腕は
    `assets/editor/theme/gg/tokens.json`（GG final の取り込み。`yellowBudget`）から読む。
  - tokens に数値が無い項目（起動画面の画素比の上限、弱い黄の判定、AA の許容）は
    `tools/splash_av_limits.json` に出典つきで置く（このファイルがこの検査の「数値の置き場」。Python には書かない）。

検査（ダンプした PNG に掛ける。1 つでも違反があれば終了コード 1）:
  1. 黄の画素比      : OKLCH で C>=chromaMin かつ h∈[hueMin,hueMax] の画素 / 全画素 <= 上限（boot / edit / play）
  2. 黄の面          : 黄の画素の面（boot は弱い黄を 9x9、edit/play は強い黄を round(4*scale) 角で侵食して残る画素）= 0
                       （play は再生枠 3px・再生ボタンの面があるので、面の検査は boot と edit だけ）
  3. トークン外の色  : 画素の色が tokens の色、または 2 つのトークン色を結ぶ線分の上（AA の中間色）にあること
  4. 許可した要素    : 黄の画素がすべて、tokens の許可集合に対応する領域（regions.json）の中にあること（regions がある時だけ）
  5. 黄の線の本数    : edit モードで、黄の連結成分の数（トンボの 4 隅は 1 組と数える）<= tokens の worstCase（9）
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TOKENS = REPO_ROOT / "assets" / "editor" / "theme" / "gg" / "tokens.json"
DEFAULT_LIMITS = Path(__file__).resolve().parent / "splash_av_limits.json"

# ---------------------------------------------------------------- 色（colorlib.py と同じ式。線形化の閾値も同じ 0.03928）


def _lin(v: np.ndarray) -> np.ndarray:
    return np.where(v <= 0.03928, v / 12.92, ((np.maximum(v, 0.0) + 0.055) / 1.055) ** 2.4)


def oklch_arrays(rgb: np.ndarray):
    """(..., 3) の 0..255 → (L, C, H[度])。"""
    c = rgb.astype(np.float64) / 255.0
    r, g, b = _lin(c[..., 0]), _lin(c[..., 1]), _lin(c[..., 2])
    l = np.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b)
    m = np.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b)
    s = np.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b)
    lab_l = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s
    lab_a = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s
    lab_b = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s
    return lab_l, np.hypot(lab_a, lab_b), np.degrees(np.arctan2(lab_b, lab_a)) % 360.0


def hex_to_rgb(h: str) -> tuple[int, int, int]:
    h = h.lstrip("#")
    return int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)


def load_rgb(path_or_arr) -> np.ndarray:
    """PNG → (H, W, 3) uint8。アルファは捨てる（ダンプは不透明）。"""
    if isinstance(path_or_arr, np.ndarray):
        return path_or_arr[..., :3].astype(np.uint8)
    return np.asarray(Image.open(path_or_arr).convert("RGB"))


def yellow_mask(rgb: np.ndarray, chroma_min: float, hue_min: float, hue_max: float) -> np.ndarray:
    """色ごとに 1 回だけ OKLCH を計算する（画像は数千色しか無い）。"""
    flat = rgb.reshape(-1, 3)
    key = (flat[:, 0].astype(np.int64) << 16) | (flat[:, 1].astype(np.int64) << 8) | flat[:, 2]
    uniq, inv = np.unique(key, return_inverse=True)
    u_rgb = np.stack([(uniq >> 16) & 255, (uniq >> 8) & 255, uniq & 255], axis=1)
    _, c, h = oklch_arrays(u_rgb)
    ok = (c >= chroma_min) & (h >= hue_min) & (h <= hue_max)
    return ok[inv].reshape(rgb.shape[:2])


# ---------------------------------------------------------------- 形態演算（numpy だけ。scipy に依存しない）


def erode(mask: np.ndarray, k: int) -> np.ndarray:
    """k×k の窓がすべて True の画素だけ残す。縁は端の値で延長する（PIL の MinFilter と同じ）。"""
    if k <= 1:
        return mask.copy()
    pad = k // 2
    p = np.pad(mask.astype(np.int32), ((pad, k - 1 - pad), (pad, k - 1 - pad)), mode="edge")
    ii = np.zeros((p.shape[0] + 1, p.shape[1] + 1), dtype=np.int64)
    ii[1:, 1:] = p.cumsum(0).cumsum(1)
    h, w = mask.shape
    s = ii[k:k + h, k:k + w] - ii[0:h, k:k + w] - ii[k:k + h, 0:w] + ii[0:h, 0:w]
    return s == k * k


def dilate(mask: np.ndarray, r: int) -> np.ndarray:
    if r <= 0:
        return mask.copy()
    inv = erode(~mask, 2 * r + 1)
    return ~inv


def components(mask: np.ndarray):
    """8 近傍の連結成分。[(ys, xs)] を返す（黄の画素は少ないので素直な走査で足りる）。"""
    ys, xs = np.nonzero(mask)
    if ys.size == 0:
        return []
    h, w = mask.shape
    label = np.full((h, w), -1, dtype=np.int32)
    out = []
    for y0, x0 in zip(ys.tolist(), xs.tolist()):
        if label[y0, x0] >= 0:
            continue
        idx = len(out)
        stack = [(y0, x0)]
        label[y0, x0] = idx
        py, px = [], []
        while stack:
            y, x = stack.pop()
            py.append(y)
            px.append(x)
            for dy in (-1, 0, 1):
                yy = y + dy
                if yy < 0 or yy >= h:
                    continue
                for dx in (-1, 0, 1):
                    xx = x + dx
                    if 0 <= xx < w and mask[yy, xx] and label[yy, xx] < 0:
                        label[yy, xx] = idx
                        stack.append((yy, xx))
        out.append((np.array(py), np.array(px)))
    return out


# ---------------------------------------------------------------- 方針（tokens.json と splash_av_limits.json から読む）

# 許可する要素の言い回し（tokens の yellowBudget.boot[*].where の語）→ 領域名（regions.json のキー）。
# 語が tokens から消えたら、その要素は許可集合から外れる（= 領域を渡してもそこの黄は違反になる）。
_ELEMENT_WORDS = (
    ("トンボ", "tombo"),
    ("校正三角", "lockTriangle"),
    ("指針", "needle"),     # 残像線もここに含まれる（『指針・指針の残像線』）
    ("ハブ", "hub"),
    ("上線", "stepOverline"),
)


@dataclass
class YellowPolicy:
    chroma_min: float
    hue_min: float
    hue_max: float
    edit_ratio_pct: float
    play_ratio_pct: float
    boot_ratio_pct: float
    edit_max_lines: int
    face_window_px: int            # edit / play: 強い黄の面の窓（1x の値。DPI で round(×scale)）
    weak_chroma_min: float         # boot: 弱い黄（光彩も拾う）
    weak_hue_min: float
    weak_hue_max: float
    boot_face_window_px: int       # boot: 弱い黄の面の窓（1x の値）
    tombo_arms_px: list[int]       # 3D 選択のトンボ / タイルのトンボの腕（1x）
    allowed_boot: list[str]        # 領域名
    token_rgbs: np.ndarray         # (N, 3) float
    aa_tolerance: float            # トークン色の線分からの許容距離（RGB の距離）
    limits_source: str = ""

    def scaled(self, v: float, scale: float) -> int:
        return max(1, int(round(v * scale)))


def _walk_values(o, out: set, skip_keys=("note",)):
    if isinstance(o, dict):
        for k, v in o.items():
            if k == "value" and isinstance(v, str):
                for m in re.findall(r"#[0-9A-Fa-f]{6}(?:[0-9A-Fa-f]{2})?\b", v):
                    out.add(m[:7].upper())
            elif k not in skip_keys:
                _walk_values(v, out, skip_keys)
    elif isinstance(o, list):
        for v in o:
            _walk_values(v, out, skip_keys)


def load_policy(tokens_path: Path = DEFAULT_TOKENS, limits_path: Path = DEFAULT_LIMITS) -> YellowPolicy:
    tok = json.loads(Path(tokens_path).read_text(encoding="utf-8"))
    lim = json.loads(Path(limits_path).read_text(encoding="utf-8"))
    yb = tok["yellowBudget"]
    zone = yb["yellowZone"]
    tests = "\n".join(yb["tests"])

    def need(pattern: str, what: str) -> float:
        m = re.search(pattern, tests)
        if not m:
            raise SystemExit(f"tokens.json の yellowBudget.tests から {what} を読めません（書き方が変わった? 要レビュー）")
        return float(m.group(1))

    edit_pct = need(r"編集モード\s*≤\s*([0-9.]+)\s*%", "編集モードの画素比の上限")
    play_pct = need(r"再生モード\s*≤\s*([0-9.]+)\s*%", "再生モードの画素比の上限")
    face_min_side = int(need(r"短辺\s*≥\s*(\d+)\s*px", "黄の塗り矩形の短辺"))
    m = re.search(r"線\s*(\d+)\s*箇所", yb["screenCap"]["worstCase"])
    if not m:
        raise SystemExit("tokens.json の yellowBudget.screenCap.worstCase から黄の線の本数を読めません")
    max_lines = int(m.group(1))
    arms = [int(x) for x in re.findall(r"（(\d+)px", " ".join(l["where"] for l in yb["lines"] if l["no"] == "06"))]
    boot_where = " ".join(b["where"] for b in yb["boot"])
    allowed = [name for word, name in _ELEMENT_WORDS if word in boot_where]

    cols: set = set()
    _walk_values(tok.get("color", {}), cols)
    _walk_values(tok.get("brand", {}).get("colors", {}) and {"v": [{"value": v} for v in tok["brand"]["colors"].values()]}, cols)
    rgbs = np.array(sorted({hex_to_rgb(h) for h in cols}), dtype=np.float64)

    return YellowPolicy(
        chroma_min=zone["oklchChromaMin"], hue_min=zone["hueMin"], hue_max=zone["hueMax"],
        edit_ratio_pct=edit_pct, play_ratio_pct=play_pct, boot_ratio_pct=float(lim["bootYellowRatioPct"]),
        edit_max_lines=max_lines, face_window_px=face_min_side,
        weak_chroma_min=float(lim["weakYellow"]["chromaMin"]), weak_hue_min=float(lim["weakYellow"]["hueMin"]),
        weak_hue_max=float(lim["weakYellow"]["hueMax"]), boot_face_window_px=int(lim["bootFaceWindowPx"]),
        tombo_arms_px=arms, allowed_boot=allowed, token_rgbs=rgbs, aa_tolerance=float(lim["tokenColor"]["aaToleranceRgb"]),
        limits_source=str(lim.get("source", "")),
    )


# ---------------------------------------------------------------- 領域（許可する要素の位置。ダンプ側が書く）


def region_mask(shape, regions: dict, names: list[str], pad_px: float) -> np.ndarray:
    """regions = {名前: [[x0,y0,x1,y1] | {"circle":[cx,cy,r]} ...]}。許可された名前だけの和集合。"""
    h, w = shape
    yy, xx = np.mgrid[0:h, 0:w]
    m = np.zeros(shape, dtype=bool)
    for name in names:
        for r in regions.get(name, []):
            if isinstance(r, dict) and "circle" in r:
                cx, cy, rad = r["circle"]
                m |= (xx + 0.5 - cx) ** 2 + (yy + 0.5 - cy) ** 2 <= (rad + pad_px) ** 2
            else:
                x0, y0, x1, y1 = r
                m |= (xx + 0.5 >= x0 - pad_px) & (xx + 0.5 <= x1 + pad_px) & (yy + 0.5 >= y0 - pad_px) & (yy + 0.5 <= y1 + pad_px)
    return m


# ---------------------------------------------------------------- トークン外の色（AA の中間色は 2 色の線分の上なら許す）


def off_token_pixels(rgb: np.ndarray, token_rgbs: np.ndarray, tol: float) -> tuple[int, list]:
    """トークン色でも 2 トークンの線分上でもない画素の数と、色の例（多い順）。"""
    flat = rgb.reshape(-1, 3)
    key = (flat[:, 0].astype(np.int64) << 16) | (flat[:, 1].astype(np.int64) << 8) | flat[:, 2]
    uniq, counts = np.unique(key, return_counts=True)
    u = np.stack([(uniq >> 16) & 255, (uniq >> 8) & 255, uniq & 255], axis=1).astype(np.float64)
    T = token_rgbs
    bad = np.ones(len(u), dtype=bool)
    # 1. トークンそのもの / 近傍
    d_tok = np.sqrt(((u[:, None, :] - T[None, :, :]) ** 2).sum(-1)).min(1)
    bad &= d_tok > tol
    # 2. 2 トークンの線分（残りの色だけ。チャンクで）
    idx = np.nonzero(bad)[0]
    a_i, b_i = np.triu_indices(len(T), k=1)
    A, B = T[a_i], T[b_i]
    AB = B - A
    ab2 = (AB ** 2).sum(1) + 1e-12
    for s in range(0, len(idx), 256):
        sl = idx[s:s + 256]
        P = u[sl][:, None, :] - A[None, :, :]
        t = np.clip((P * AB[None]).sum(-1) / ab2[None], 0.0, 1.0)
        proj = A[None] + t[..., None] * AB[None]
        d = np.sqrt(((u[sl][:, None, :] - proj) ** 2).sum(-1)).min(1)
        bad[sl[d <= tol]] = False
    n = int(counts[bad].sum())
    order = np.argsort(-counts[bad])
    examples = [("#%02X%02X%02X" % tuple(int(v) for v in u[bad][i]), int(counts[bad][i])) for i in order[:5]]
    return n, examples


# ---------------------------------------------------------------- 検査本体


@dataclass
class YellowReport:
    mode: str
    width: int
    height: int
    ratio_pct: float
    ratio_limit_pct: float
    face_px: int
    off_token_px: int
    off_token_examples: list
    outside_region_px: int | None
    lines: int | None
    lines_limit: int | None
    failures: list = field(default_factory=list)
    notes: list = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.failures

    def to_json(self):
        d = dict(self.__dict__)
        d["ok"] = self.ok
        return d


def _count_lines(strict: np.ndarray, policy: YellowPolicy, scale: float) -> int:
    """黄の線の本数 = 連結成分（1px の隙間は同じ線とみなす）。トンボの隅（腕 <= tokens の腕）は 4 つで 1 組と数える。"""
    comps = components(dilate(strict, 1))
    arm = max(policy.tombo_arms_px or [0]) * scale + 3
    corner = 0
    lines = 0
    for ys, xs in comps:
        side = max(ys.max() - ys.min() + 1, xs.max() - xs.min() + 1)
        if arm > 3 and side <= arm:
            corner += 1
        else:
            lines += 1
    return lines + math.ceil(corner / 4)


def check_yellow(img, mode: str, policy: YellowPolicy, regions: dict | None = None, scale: float = 1.0,
                 region_pad_px: float = 2.0, exclude: list | None = None) -> YellowReport:
    """1 枚の画面（boot / edit / play）に黄の予算を掛ける。img は PNG のパスか (H,W,3) の配列。

    exclude: 検査から外す領域の一覧（矩形 [x0,y0,x1,y1] か {circle:[cx,cy,r]}、画素の縁を原点とする連続座標）。
    実エディタのスクリーンショットでは 3D ビューポート・ゲーム画像・サムネイル・アイコンなど描画された
    内容の画素が含まれる。その領域を渡すと、黄の画素比・面・線の本数・トークン外の色のどれにも数えない
    （画素比の分母は画面全体のまま。外した分だけ分母を小さくはしない）。
    """
    if mode not in ("boot", "edit", "play"):
        raise ValueError(mode)
    rgb = load_rgb(img)
    h, w, _ = rgb.shape
    strict = yellow_mask(rgb, policy.chroma_min, policy.hue_min, policy.hue_max)
    excl = None
    if exclude:
        excl = region_mask((h, w), {"x": list(exclude)}, ["x"], 0.0)
        strict = strict & ~excl
    ratio = float(strict.sum()) / (h * w) * 100.0
    limit = {"boot": policy.boot_ratio_pct, "edit": policy.edit_ratio_pct, "play": policy.play_ratio_pct}[mode]
    failures: list[str] = []
    notes: list[str] = []
    if ratio > limit:
        failures.append(f"黄の画素比 {ratio:.3f}% が上限 {limit}% を超えています")

    face = 0
    if mode == "boot":
        weak = yellow_mask(rgb, policy.weak_chroma_min, policy.weak_hue_min, policy.weak_hue_max)
        if excl is not None:
            weak = weak & ~excl
        face = int(erode(weak, policy.scaled(policy.boot_face_window_px, scale)).sum())
    elif mode == "edit":
        face = int(erode(strict, policy.scaled(policy.face_window_px, scale)).sum())
    else:
        notes.append("play: 再生枠 3px と再生ボタンの面があるので黄の面の検査は行わない（画素比だけ）")
    if face:
        failures.append(f"黄の面が {face}px 残っています（線・トンボ・点は窓に満たないので消えるはず）")

    if excl is not None:
        notes.append(f"exclude: {int(excl.sum())}px を検査から外した（描画内容の領域）")
        off_n, off_ex = off_token_pixels(rgb[~excl].reshape(-1, 1, 3), policy.token_rgbs, policy.aa_tolerance) if (~excl).any() else (0, [])
    else:
        off_n, off_ex = off_token_pixels(rgb, policy.token_rgbs, policy.aa_tolerance)
    if off_n:
        failures.append(f"トークン外の色の画素が {off_n}px（例 {off_ex[:3]}）")

    outside = None
    if regions is not None and mode == "boot":
        allowed = region_mask((h, w), regions, policy.allowed_boot, region_pad_px * max(1.0, scale))
        outside = int((strict & ~allowed).sum())
        if outside:
            failures.append(f"許可した要素（{'・'.join(policy.allowed_boot)}）の外に黄が {outside}px あります")
    elif mode == "boot":
        notes.append("regions なし: 黄が許可した要素の中にあるかは検査していない")

    lines = None
    if mode == "edit":
        lines = _count_lines(strict, policy, scale)
        if lines > policy.edit_max_lines:
            failures.append(f"黄の線が {lines} 本（上限 {policy.edit_max_lines}）")
    return YellowReport(mode, w, h, ratio, limit, face, off_n, off_ex, outside, lines,
                        policy.edit_max_lines if mode == "edit" else None, failures, notes)


# ---------------------------------------------------------------- SSIM（ref_boot_hero.png との比較）


def _gauss_kernel(size: int = 11, sigma: float = 1.5) -> np.ndarray:
    x = np.arange(size) - (size - 1) / 2.0
    k = np.exp(-(x ** 2) / (2 * sigma ** 2))
    return k / k.sum()


def _filt(a: np.ndarray, k: np.ndarray) -> np.ndarray:
    n = len(k)
    h, w = a.shape
    tmp = sum(k[i] * a[:, i:w - n + 1 + i] for i in range(n))
    return sum(k[i] * tmp[i:h - n + 1 + i, :] for i in range(n))


def ssim_gray(a: np.ndarray, b: np.ndarray) -> float:
    """標準の SSIM（ガウス窓 11, σ=1.5, K1=0.01, K2=0.03, 輝度 = Rec.601）。a, b は (H,W,3) uint8。"""
    def gray(x):
        x = x.astype(np.float64)
        return 0.299 * x[..., 0] + 0.587 * x[..., 1] + 0.114 * x[..., 2]
    x, y = gray(a), gray(b)
    if x.shape != y.shape:
        raise ValueError(f"大きさが違う: {x.shape} と {y.shape}")
    k = _gauss_kernel()
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mx, my = _filt(x, k), _filt(y, k)
    sxx = _filt(x * x, k) - mx * mx
    syy = _filt(y * y, k) - my * my
    sxy = _filt(x * y, k) - mx * my
    s = ((2 * mx * my + c1) * (2 * sxy + c2)) / ((mx * mx + my * my + c1) * (sxx + syy + c2))
    return float(s.mean())


# ---------------------------------------------------------------- CLI


def _load_regions(path: Path | None):
    if path is None:
        return None
    d = json.loads(path.read_text(encoding="utf-8"))
    return d.get("regions", d)


def _print_report(name: str, r: YellowReport):
    print(f"[{'OK' if r.ok else 'NG'}] {name}  {r.mode} {r.width}x{r.height}  黄 {r.ratio_pct:.3f}%/{r.ratio_limit_pct}%  面 {r.face_px}px  トークン外 {r.off_token_px}px"
          + (f"  外側 {r.outside_region_px}px" if r.outside_region_px is not None else "")
          + (f"  線 {r.lines}/{r.lines_limit}" if r.lines is not None else ""))
    for f in r.failures:
        print(f"     NG: {f}")
    for n in r.notes:
        print(f"     note: {n}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tokens", type=Path, default=DEFAULT_TOKENS)
    ap.add_argument("--limits", type=Path, default=DEFAULT_LIMITS)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("yellow")
    p.add_argument("png", nargs="+", type=Path)
    p.add_argument("--mode", choices=["boot", "edit", "play"], required=True)
    p.add_argument("--regions", type=Path)
    p.add_argument("--scale", type=float, default=1.0)
    p.add_argument("--exclude", action="append", default=[], metavar="X0,Y0,X1,Y1",
                   help="検査から外す矩形（3D ビューポートなど。複数回指定可）")
    p.add_argument("--exclude-json", type=Path, help="外す領域の JSON（矩形か {circle:[cx,cy,r]} の配列、または {\"exclude\": [...]}）")
    p = sub.add_parser("dir")
    p.add_argument("dump", type=Path)
    p.add_argument("--mode", choices=["boot", "edit", "play"], default="boot")
    p.add_argument("--scale", type=float)
    sub.add_parser("policy")
    p = sub.add_parser("ssim")
    p.add_argument("actual", type=Path)
    p.add_argument("reference", type=Path)
    p.add_argument("--min", type=float, default=0.85)
    a = ap.parse_args(argv)

    if a.cmd == "ssim":
        s = ssim_gray(load_rgb(a.actual), load_rgb(a.reference))
        print(f"SSIM {s:.4f} (min {a.min})")
        return 0 if s >= a.min else 1
    pol = load_policy(a.tokens, a.limits)
    if a.cmd == "policy":
        d = {k: v for k, v in pol.__dict__.items() if k != "token_rgbs"}
        d["tokenColorCount"] = int(len(pol.token_rgbs))
        print(json.dumps(d, ensure_ascii=False, indent=1))
        return 0
    if a.cmd == "yellow":
        regions = _load_regions(a.regions)
        excl = [[float(v) for v in e.split(",")] for e in a.exclude]
        if a.exclude_json:
            d = json.loads(a.exclude_json.read_text(encoding="utf-8"))
            excl += d.get("exclude", []) if isinstance(d, dict) else d
        rc = 0
        for f in a.png:
            r = check_yellow(f, a.mode, pol, regions, a.scale, exclude=excl or None)
            _print_report(f.name, r)
            rc |= 0 if r.ok else 1
        return rc
    if a.cmd == "dir":
        av = a.dump / "av.json"
        regions, scale = None, a.scale
        if av.exists():
            meta = json.loads(av.read_text(encoding="utf-8"))
            regions = meta.get("regions")
            scale = scale if scale is not None else float(meta.get("dpiScale", 1.0))
        scale = 1.0 if scale is None else scale
        pngs = sorted(a.dump.glob("*.png"))
        if not pngs:
            print("PNG がありません", file=sys.stderr)
            return 1
        rc = 0
        for f in pngs:
            r = check_yellow(f, a.mode, pol, regions, scale)
            _print_report(f.name, r)
            rc |= 0 if r.ok else 1
        return rc
    return 2


if __name__ == "__main__":
    sys.exit(main())
