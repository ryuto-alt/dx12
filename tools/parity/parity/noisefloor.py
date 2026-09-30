"""ノイズ床の推定。

同じシーン・同じカメラを「異なるシード」で 2 回(以上)レンダリングした画像同士を比べると、
「これ以下の差は区別できない(ノイズ由来)」という床が出る。合格しきい値は floor の k 倍(既定 1.5)以上に置く
(設計書 §3.7: しきい値 = max(固定値, ノイズ床 x 1.5))。

注意:
  * 2 枚の独立ノイズ画像の差は、「ノイズ画像 vs 真値」の差の sqrt(2) 倍になる。Uno(決定論でノイズ 0)と
    パストレーサーの基準を比べる本番の比較では、実際のノイズは床の約 1/sqrt(2)。床をそのまま使うのは
    保守的(緩め)な側の誤り。correction="single" で 1/sqrt(2) を掛けられる。
  * 床は「その spp・その解像度・その基準の種別」で決まる。基準の設定(spp 等)を変えたら測り直す。
"""
from __future__ import annotations

import itertools
import json
import math
from pathlib import Path

from . import metrics as M
from .imgio import Img
from .tonemap import align_pair

FLOOR_METRICS = ["flip_ldr_mean", "flip_ldr_p95", "ssim", "psnr", "de2000_median", "hist_emd_ev", "lum_mean_ev",
                 "hue_abs_median_deg", "flip_hdr_mean"]
SQRT2 = math.sqrt(2.0)


def measure(images: list[Img], tonemap: str = "engine_aces", exposure=None, pairs: str = "consecutive",
            correction: str = "pair", metrics: list[str] | None = None) -> dict:
    """images = 同一シーンを異なるシードで撮った N(>=2) 枚。"""
    if len(images) < 2:
        raise ValueError("ノイズ床には同じシーンを異なるシードで撮った画像が 2 枚以上要る")
    if correction not in ("pair", "single"):
        raise ValueError("correction は pair / single")
    idx = list(itertools.combinations(range(len(images)), 2)) if pairs == "all" else [(i, i + 1) for i in range(len(images) - 1)]
    mets = M.normalize_metrics(metrics)
    rows = []
    for i, j in idx:
        al = align_pair(images[i], images[j], tonemap, exposure)
        m2 = list(mets)
        if metrics is None and al.ref_linear is not None and "flip-hdr" not in m2:
            m2.append("flip-hdr")
        maps = M.compute_maps(al, m2)
        s = M.summarize(maps, None, m2)
        rows.append({"pair": [i, j], **{k: v for k, v in s.items() if k in FLOOR_METRICS}})
    floors: dict[str, float] = {}
    for key in FLOOR_METRICS:
        vals = [r[key] for r in rows if key in r]
        if not vals:
            continue
        d = M.metric_direction(key)
        if d == "higher":
            worst = min(vals)
        elif d == "zero":
            worst = max(vals, key=abs)
        else:
            worst = max(vals)
        floors[key] = worst
    if correction == "single":
        for key, v in list(floors.items()):
            d = M.metric_direction(key)
            if key == "ssim":
                floors[key] = 1.0 - (1.0 - v) / SQRT2
            elif key == "psnr":
                floors[key] = v + 20 * math.log10(SQRT2)
            else:
                floors[key] = v / SQRT2
    return {"floors": floors, "pairs": rows, "n_images": len(images), "correction": correction,
            "tonemap": tonemap, "size": list(images[0].size),
            "note": "床 = 異なるシードの画像同士の差(最悪ペア)。しきい値は max(固定値, 床 x k)"}


def save(path: str | Path, result: dict, meta: dict | None = None) -> Path:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    body = dict(result)
    if meta:
        body["meta"] = meta
    p.write_text(json.dumps(body, indent=2, ensure_ascii=False), encoding="utf-8")
    return p


def load(path: str | Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def effective_limit(key: str, op: str, fixed: float, floors: dict | None, k: float = 1.5) -> tuple[float, bool]:
    """しきい値 = 固定値とノイズ床 x k の緩い方。戻り値 = (使う限界値, 床で緩めたか)。"""
    if not floors or key not in floors:
        return fixed, False
    f = floors[key]
    if op in ("max", "abs_max"):
        lim = max(fixed, k * abs(f))
        return lim, lim > fixed
    if op == "min":
        if key == "ssim":
            lim = min(fixed, 1.0 - k * (1.0 - f))
        elif key == "psnr":
            lim = min(fixed, f - 20.0 * math.log10(k))
        else:
            return fixed, False
        return lim, lim < fixed
    return fixed, False
