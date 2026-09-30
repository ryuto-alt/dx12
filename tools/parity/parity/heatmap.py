"""局所ヒートマップ・差分画像・コンタクトシート・タイル集計。PIL と numpy だけで描く(matplotlib 不要)。"""
from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from . import color
from .imgio import save_png

# magma の代表 9 点(0..1 を等間隔)。FLIP 公式の可視化と同じ magma 系。
_MAGMA = np.array([
    (0, 0, 4), (28, 16, 68), (79, 18, 123), (129, 37, 129), (181, 54, 122),
    (229, 80, 100), (251, 135, 97), (254, 194, 135), (252, 253, 191)], dtype=np.float32) / 255.0


def magma(v: np.ndarray) -> np.ndarray:
    """0..1 のスカラー場 -> 0..1 の RGB(HxWx3)。"""
    v = np.clip(np.asarray(v, dtype=np.float32), 0.0, 1.0) * (len(_MAGMA) - 1)
    i = np.minimum(v.astype(np.int32), len(_MAGMA) - 2)
    f = (v - i)[..., None]
    return _MAGMA[i] * (1 - f) + _MAGMA[i + 1] * f


def _font(size: int = 14):
    for name in ("meiryo.ttc", "YuGothM.ttc", "msgothic.ttc", "segoeui.ttf", "arial.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def add_colorbar(rgb: np.ndarray, vmax: float, label: str = "FLIP error", bar_h: int = 60) -> np.ndarray:
    """ヒートマップの下にカラーバー(0 .. vmax)を付ける。"""
    h, w, _ = rgb.shape
    canvas = np.zeros((h + bar_h, w, 3), dtype=np.float32) + 0.08
    canvas[:h] = rgb
    x0, x1 = int(w * 0.06), int(w * 0.94)
    grad = magma(np.linspace(0, 1, max(2, x1 - x0)))[None, :, :].repeat(14, axis=0)
    canvas[h + 6:h + 20, x0:x0 + grad.shape[1]] = grad
    im = Image.fromarray(np.clip(canvas * 255 + 0.5, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    f = _font(13)
    for k in range(5):
        x = x0 + int((x1 - x0 - 1) * k / 4)
        d.line([(x, h + 20), (x, h + 24)], fill=(220, 220, 220))
        txt = f"{vmax * k / 4:.2f}"
        d.text((x - 10, h + 25), txt, fill=(220, 220, 220), font=f)
    d.text((x0, h + 40), label, fill=(160, 160, 160), font=f)
    return np.asarray(im, dtype=np.float32) / 255.0


def save_error_heatmap(path: str | Path, err: np.ndarray, vmax: float = 1.0, label: str = "FLIP error", bar: bool = True) -> Path:
    rgb = magma(err / max(vmax, 1e-9))
    if bar:
        rgb = add_colorbar(rgb, vmax, label)
    return save_png(path, rgb)


def diff_image(ref_display: np.ndarray, test_display: np.ndarray, gain: float = 4.0) -> np.ndarray:
    """|ref - test| を gain 倍して見やすくしたグレー画像(表示空間の差)。"""
    d = np.abs(np.asarray(ref_display, np.float32) - np.asarray(test_display, np.float32)) * gain
    return np.clip(d, 0, 1)


def signed_luma_diff(ref_display: np.ndarray, test_display: np.ndarray, gain: float = 4.0) -> np.ndarray:
    """test が明るければ赤、暗ければ青(輝度差)。どちら向きにずれているかが分かる。"""
    d = (color.luma_display(test_display) - color.luma_display(ref_display)) * gain
    out = np.zeros(d.shape + (3,), dtype=np.float32) + 0.06
    pos, neg = np.clip(d, 0, 1), np.clip(-d, 0, 1)
    out[..., 0] += pos * 0.94
    out[..., 1] += (pos + neg) * 0.15
    out[..., 2] += neg * 0.94
    return np.clip(out, 0, 1)


def tile_stats(err: np.ndarray, tile: int = 16) -> np.ndarray:
    """タイルごとの平均(端は切り捨て)。"""
    h, w = err.shape
    th, tw = h // tile, w // tile
    if th == 0 or tw == 0:
        return err.mean(keepdims=True).reshape(1, 1)
    return err[:th * tile, :tw * tile].reshape(th, tile, tw, tile).mean(axis=(1, 3))


def worst_tiles(err: np.ndarray, tile: int = 16, top: int = 10) -> list[dict]:
    """FLIP 平均が悪い上位タイルと画素座標(レポートで「どこが違うか」を指す)。"""
    ts = tile_stats(err, tile)
    idx = np.argsort(ts, axis=None)[::-1][:top]
    out = []
    for i in idx:
        ty, tx = divmod(int(i), ts.shape[1])
        out.append({"x": tx * tile, "y": ty * tile, "w": tile, "h": tile, "mean": float(ts[ty, tx])})
    return out


def _label(img: np.ndarray, text: str) -> np.ndarray:
    im = Image.fromarray(np.clip(img * 255 + 0.5, 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    f = _font(15)
    tw = d.textlength(text, font=f)
    d.rectangle([0, 0, tw + 12, 22], fill=(0, 0, 0))
    d.text((6, 3), text, fill=(255, 255, 255), font=f)
    return np.asarray(im, dtype=np.float32) / 255.0


def contact_sheet(panels: list[tuple[str, np.ndarray]], path: str | Path, max_width: int = 2400, gap: int = 6) -> Path:
    """[(ラベル, 0..1 RGB), ...] を横に並べる(UE | Uno | ヒートマップ)。全部同じ高さへ縮める。"""
    n = len(panels)
    w_each = min(max(p.shape[1] for _, p in panels), (max_width - gap * (n - 1)) // n)
    scale = min(1.0, w_each / max(p.shape[1] for _, p in panels))
    tiles = []
    for label, p in panels:
        im = Image.fromarray(np.clip(p * 255 + 0.5, 0, 255).astype(np.uint8))
        tw, th = max(1, int(im.width * scale)), max(1, int(im.height * scale))
        im = im.resize((tw, th), Image.BOX if scale < 1 else Image.NEAREST)
        tiles.append(_label(np.asarray(im, dtype=np.float32) / 255.0, label))
    hh = max(t.shape[0] for t in tiles)
    ww = sum(t.shape[1] for t in tiles) + gap * (n - 1)
    canvas = np.full((hh, ww, 3), 0.1, dtype=np.float32)
    x = 0
    for t in tiles:
        canvas[:t.shape[0], x:x + t.shape[1]] = t
        x += t.shape[1] + gap
    return save_png(path, canvas)
