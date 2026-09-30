"""画像の読み書き。色空間を必ず明示する(sRGB エンコードか、リニアか)。

Img.kind:
  "display" = 表示参照。PNG の画素値そのもの(0..1、eotf でエンコード済み)。8bit / 16bit PNG。
  "linear"  = シーン参照のリニア RGB(Rec.709 原色。1.0 を超えてよい)。PFM / EXR / HDR。

  PNG を linear として読みたいとき(法線などの特殊用途)は colorspace="linear" を明示する。
  逆に EXR を sRGB として読むことはできない(意味が無い)。

eotf は display 画像をリニアへ戻すときの式: "srgb"(既定。外部の PNG は基本これ)か
"gamma22"(Uno のポストは ACES の後に pow(1/2.2) を掛けるので、Uno の PNG は厳密には gamma22)。
"""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from PIL import Image

from . import color

LDR_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff"}
HDR_EXT = {".pfm", ".exr", ".hdr"}


class ImageError(Exception):
    pass


class SizeMismatch(ImageError):
    pass


@dataclass
class Img:
    rgb: np.ndarray                     # float32, HxWx3
    kind: str = "display"               # "display" | "linear"
    eotf: str = "srgb"                  # display のときだけ意味がある
    path: str | None = None
    meta: dict = field(default_factory=dict)

    @property
    def height(self) -> int:
        return int(self.rgb.shape[0])

    @property
    def width(self) -> int:
        return int(self.rgb.shape[1])

    @property
    def size(self) -> tuple[int, int]:
        return (self.width, self.height)

    def to_linear(self) -> np.ndarray:
        """リニア RGB(float32)。display なら eotf で戻す。"""
        if self.kind == "linear":
            return self.rgb
        return color.decode(self.rgb, self.eotf)

    def copy_with(self, rgb: np.ndarray, **kw) -> "Img":
        d = dict(rgb=rgb.astype(np.float32, copy=False), kind=self.kind, eotf=self.eotf, path=self.path, meta=dict(self.meta))
        d.update(kw)
        return Img(**d)


# ── 読み込み ────────────────────────────────────────────────────────────────

def _png_bit_depth(path: Path) -> int | None:
    try:
        with open(path, "rb") as f:
            head = f.read(26)
        if head[:8] == b"\x89PNG\r\n\x1a\n":
            return head[24]
    except OSError:
        pass
    return None


def _cv2():
    # OpenEXR は既定で無効。import より前に環境変数を立てる必要がある。
    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    try:
        import cv2  # noqa: WPS433
        return cv2
    except ImportError:
        return None


def _read_cv2(path: Path) -> np.ndarray:
    cv2 = _cv2()
    if cv2 is None:
        raise ImageError(f"{path.suffix} の読み込みには opencv-python-headless が要る(pip install opencv-python-headless)")
    buf = np.fromfile(str(path), dtype=np.uint8)   # 日本語パスでも読めるように fromfile
    a = cv2.imdecode(buf, cv2.IMREAD_UNCHANGED | cv2.IMREAD_ANYDEPTH | cv2.IMREAD_ANYCOLOR)
    if a is None:
        raise ImageError(f"画像を読めない: {path}")
    return a


def _to_rgb3(a: np.ndarray, bgr: bool) -> np.ndarray:
    if a.ndim == 2:
        a = np.stack([a] * 3, axis=-1)
    elif a.shape[2] == 1:
        a = np.repeat(a, 3, axis=2)
    elif a.shape[2] >= 3:
        a = a[:, :, :3]
        if bgr:
            a = a[:, :, ::-1]
    elif a.shape[2] == 2:                     # gray + alpha
        a = np.repeat(a[:, :, :1], 3, axis=2)
    return np.ascontiguousarray(a)


def read_exr(path: Path) -> np.ndarray:
    """OpenEXR(公式バインディング)を優先。無ければ OpenCV(ただし pip 版 OpenCV 5 は EXR 非対応)。"""
    try:
        import OpenEXR
    except ImportError:
        OpenEXR = None
    if OpenEXR is not None:
        chans = OpenEXR.File(str(path)).channels()
        if "RGB" in chans:
            a = np.asarray(chans["RGB"].pixels, dtype=np.float32)
        elif "RGBA" in chans:
            a = np.asarray(chans["RGBA"].pixels, dtype=np.float32)[:, :, :3]
        elif "Y" in chans:
            a = np.asarray(chans["Y"].pixels, dtype=np.float32)
        else:
            raise ImageError(f"EXR に RGB チャンネルが無い({list(chans)}): {path}")
        return _to_rgb3(a, bgr=False)
    a = _read_cv2(path)
    return _to_rgb3(a.astype(np.float32), bgr=True)


def write_exr(path: Path | str, linear: np.ndarray) -> None:
    import OpenEXR
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    hdr = {"compression": OpenEXR.ZIP_COMPRESSION, "type": OpenEXR.scanlineimage}
    OpenEXR.File(hdr, {"RGB": np.ascontiguousarray(linear, dtype=np.float32)}).write(str(path))


def read_pfm(path: Path) -> np.ndarray:
    with open(path, "rb") as f:
        hdr = f.readline().strip()
        if hdr not in (b"PF", b"Pf"):
            raise ImageError(f"PFM ではない: {path}")
        ch = 3 if hdr == b"PF" else 1
        dims = f.readline().split()
        while len(dims) < 2:
            dims += f.readline().split()
        w, h = int(dims[0]), int(dims[1])
        scale = float(f.readline().strip())
        dt = "<f4" if scale < 0 else ">f4"
        data = np.frombuffer(f.read(w * h * ch * 4), dtype=dt)
    if data.size != w * h * ch:
        raise ImageError(f"PFM のサイズが合わない: {path}")
    a = data.reshape(h, w, ch) if ch == 3 else data.reshape(h, w)
    a = np.flipud(a)                          # PFM は下から上
    return _to_rgb3(a.astype(np.float32), bgr=False)


def write_pfm(path: Path | str, linear: np.ndarray) -> None:
    a = np.asarray(linear, dtype="<f4")
    if a.ndim == 2:
        a = np.stack([a] * 3, axis=-1)
    h, w, _ = a.shape
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"PF\n")
        f.write(f"{w} {h}\n".encode())
        f.write(b"-1.0\n")
        f.write(np.flipud(a).tobytes())


def load_image(path: str | Path, colorspace: str = "auto", eotf: str | None = None) -> Img:
    """colorspace: auto | srgb | gamma22 | linear。

    auto = PNG/JPG/BMP/TIF は sRGB(表示参照)、PFM/EXR/HDR はリニア。
    """
    p = Path(path)
    if not p.exists():
        raise ImageError(f"画像が無い: {p}")
    ext = p.suffix.lower()
    cs = colorspace.lower()
    if cs not in ("auto", "srgb", "gamma22", "linear"):
        raise ImageError(f"色空間が不明: {colorspace}(auto / srgb / gamma22 / linear)")

    if ext in HDR_EXT:
        if cs in ("srgb", "gamma22"):
            raise ImageError(f"{ext} はリニア HDR。{colorspace} としては読めない(colorspace=linear か auto)")
        if ext == ".pfm":
            rgb = read_pfm(p)
        elif ext == ".exr":
            rgb = read_exr(p)
        else:
            a = _read_cv2(p)
            rgb = _to_rgb3(a.astype(np.float32), bgr=True)
        rgb = np.nan_to_num(rgb.astype(np.float32), nan=0.0, posinf=65504.0, neginf=0.0)
        return Img(rgb=rgb, kind="linear", path=str(p), meta={"source": ext})

    if ext not in LDR_EXT:
        raise ImageError(f"未対応の拡張子: {ext}")
    depth = _png_bit_depth(p) if ext == ".png" else None
    if depth == 16:
        a = _read_cv2(p)
        rgb = _to_rgb3(a.astype(np.float32) / 65535.0, bgr=True)
        bits = 16
    else:
        im = Image.open(p)
        if im.mode == "I;16":
            rgb = _to_rgb3(np.asarray(im, dtype=np.float32) / 65535.0, bgr=False)
            bits = 16
        else:
            im = im.convert("RGB") if im.mode not in ("RGB",) else im
            rgb = np.asarray(im, dtype=np.float32) / 255.0
            bits = 8
    if cs == "linear":
        return Img(rgb=rgb, kind="linear", path=str(p), meta={"source": ext, "bits": bits, "note": "PNG をリニアとして読んだ"})
    e = eotf or ("gamma22" if cs == "gamma22" else "srgb")
    return Img(rgb=rgb, kind="display", eotf=e, path=str(p), meta={"source": ext, "bits": bits})


# ── 書き込み ────────────────────────────────────────────────────────────────

def to_u8(display: np.ndarray) -> np.ndarray:
    return np.clip(np.rint(np.asarray(display, dtype=np.float32) * 255.0), 0, 255).astype(np.uint8)


def save_png(path: str | Path, display: np.ndarray) -> Path:
    """display は 0..1(エンコード済み)の HxWx3 か HxW。"""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    a = to_u8(display)
    Image.fromarray(a).save(p)
    return p


# ── サイズ検査 ──────────────────────────────────────────────────────────────

def ensure_same_size(a: Img, b: Img, policy: str = "error", aspect_tol: float = 0.01) -> tuple[Img, Img, dict]:
    """policy:
      error  = 違えば SizeMismatch(既定。黙って比べない)
      resize = b を a の大きさへ(縦横比が aspect_tol 以内のときだけ。縮小は面積平均・拡大は Lanczos)
      crop   = 中央で共通の大きさへ切り出す
    戻り値の dict は「何をしたか」(レポートに載せる)。
    """
    if a.size == b.size:
        return a, b, {"sizePolicy": "same", "size": list(a.size)}
    msg = f"画像の大きさが違う: {a.width}x{a.height} と {b.width}x{b.height}"
    if policy == "error":
        raise SizeMismatch(msg + "(--size-policy resize か crop で合わせられる)")
    if policy == "crop":
        w, h = min(a.width, b.width), min(a.height, b.height)

        def cc(im: Img) -> Img:
            x0, y0 = (im.width - w) // 2, (im.height - h) // 2
            return im.copy_with(im.rgb[y0:y0 + h, x0:x0 + w])
        return cc(a), cc(b), {"sizePolicy": "crop", "from": [list(a.size), list(b.size)], "size": [w, h]}
    if policy == "resize":
        ra, rb = a.width / a.height, b.width / b.height
        if abs(ra - rb) / ra > aspect_tol:
            raise SizeMismatch(msg + f"。縦横比も違う({ra:.4f} と {rb:.4f})ので resize では合わせない(crop を使う)")
        return a, b.copy_with(resize(b.rgb, a.width, a.height)), {
            "sizePolicy": "resize", "from": [list(a.size), list(b.size)], "size": list(a.size),
            "note": "b を a の大きさへリサンプル(色空間の値のまま。解像度差は指標に影響するので参考扱い)"}
    raise ImageError(f"size policy が不明: {policy}")


def resize(rgb: np.ndarray, w: int, h: int) -> np.ndarray:
    """float 画像のリサイズ。縮小 = 面積平均(BOX)、拡大 = Lanczos。チャンネルごとに float32 のまま処理。"""
    src_h, src_w = rgb.shape[:2]
    down = (w * h) < (src_w * src_h)
    method = Image.BOX if down else Image.LANCZOS
    out = np.empty((h, w, 3), dtype=np.float32)
    for c in range(3):
        out[:, :, c] = np.asarray(Image.fromarray(np.ascontiguousarray(rgb[:, :, c]), mode="F").resize((w, h), method))
    return out
