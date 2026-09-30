"""領域マスク(「空」「床」「植生」などの領域ごとに指標を出す)。

マスク PNG の規約:
  * 2 値マスク: 明るさ 128 以上(RGBA でアルファが全部 255 でなければアルファ)の画素が領域の内側。
  * ラベルマップ: 1 枚の PNG に領域ごとの色を塗り分け、シーン仕様で領域ごとに color=[r,g,b] を指定する
    (完全一致。アンチエイリアスのある塗りは使わない)。
大きさが画像と違うときは最近傍で合わせる(縦横比が違えばエラー)。
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image


class MaskError(Exception):
    pass


def load_mask(path: str | Path, size: tuple[int, int], color: list[int] | None = None) -> np.ndarray:
    p = Path(path)
    if not p.exists():
        raise MaskError(f"マスク画像が無い: {p}")
    im = Image.open(p)
    w, h = size
    if im.size != (w, h):
        ra, rb = im.width / im.height, w / h
        if abs(ra - rb) / rb > 0.01:
            raise MaskError(f"マスクの縦横比が画像と違う: マスク {im.size} / 画像 {(w, h)}")
        im = im.resize((w, h), Image.NEAREST)
    if color is not None:
        a = np.asarray(im.convert("RGB"), dtype=np.uint8)
        m = np.all(a == np.array(color[:3], dtype=np.uint8), axis=-1)
        if not m.any():
            raise MaskError(f"ラベルマップに色 {color} の画素が無い: {p}")
        return m
    if im.mode in ("RGBA", "LA") and np.asarray(im.getchannel("A")).min() < 255:
        m = np.asarray(im.getchannel("A")) >= 128
    else:
        m = np.asarray(im.convert("L")) >= 128
    if not m.any():
        raise MaskError(f"マスクが空(全部外側): {p}")
    return m


def load_regions(regions: list[dict], size: tuple[int, int], base_dir: Path | None = None) -> dict[str, np.ndarray]:
    """[{name, mask, color?}] -> {name: bool HxW}。mask は base_dir からの相対でもよい。"""
    out: dict[str, np.ndarray] = {}
    for r in regions or []:
        mp = Path(r["mask"])
        if not mp.is_absolute() and base_dir is not None:
            mp = base_dir / mp
        out[r["name"]] = load_mask(mp, size, r.get("color"))
    return out
