"""DDS(DX10 拡張ヘッダ)の最小の書き出し/読み込み。非圧縮のデータテクスチャ専用。

VFX の焼き込み資産は「色」ではなく「データ」(温度、不透明度、モーションベクトル、カール場)なので、
すべてリニア(sRGB ではない)・非圧縮・ミップ無しで書く。BC 圧縮は MV と 6-way で量子化誤差が縞になるため使わない。
"""
import struct

import numpy as np

DXGI_R16G16B16A16_SNORM = 13
DXGI_R8G8B8A8_UNORM = 28
DXGI_R16G16_SNORM = 37

# (要素数, 1 要素のバイト, numpy の dtype)
_FORMATS = {
    DXGI_R8G8B8A8_UNORM: (4, 1, np.dtype("<u1")),
    DXGI_R16G16_SNORM: (2, 2, np.dtype("<i2")),
    DXGI_R16G16B16A16_SNORM: (4, 2, np.dtype("<i2")),
}

_MAGIC = b"DDS "
_DDSD_CAPS, _DDSD_HEIGHT, _DDSD_WIDTH, _DDSD_PITCH, _DDSD_PIXELFORMAT, _DDSD_DEPTH = 0x1, 0x2, 0x4, 0x8, 0x1000, 0x800000
_DDPF_FOURCC = 0x4
_DDSCAPS_TEXTURE = 0x1000
_DDSCAPS2_VOLUME = 0x200000


def snorm16(x):
    """-1..1 の浮動小数を 16bit SNORM の整数へ(最近接、-32768 は使わない)。"""
    return np.clip(np.rint(np.asarray(x, np.float64) * 32767.0), -32767, 32767).astype("<i2")


def unorm8(x):
    return np.clip(np.rint(np.asarray(x, np.float64) * 255.0), 0, 255).astype("<u1")


def write_dds(path, dxgi, data):
    """data: 2D は (H, W, C)、3D は (D, H, W, C)。dtype は形式に合わせる。"""
    comps, bpe, dt = _FORMATS[dxgi]
    data = np.ascontiguousarray(data)
    if data.dtype != dt:
        raise ValueError(f"dtype {data.dtype} != {dt}")
    is3d = data.ndim == 4
    if data.shape[-1] != comps:
        raise ValueError("要素数が形式と合わない")
    depth, (h, w) = (data.shape[0], data.shape[1:3]) if is3d else (1, data.shape[:2])
    pitch = w * comps * bpe
    flags = _DDSD_CAPS | _DDSD_HEIGHT | _DDSD_WIDTH | _DDSD_PITCH | _DDSD_PIXELFORMAT | (_DDSD_DEPTH if is3d else 0)
    caps2 = _DDSCAPS2_VOLUME if is3d else 0
    hdr = struct.pack("<4s", _MAGIC)
    hdr += struct.pack("<7I", 124, flags, h, w, pitch, depth if is3d else 0, 1)
    hdr += b"\0" * 44  # reserved1[11]
    hdr += struct.pack("<8I", 32, _DDPF_FOURCC, int.from_bytes(b"DX10", "little"), 0, 0, 0, 0, 0)
    hdr += struct.pack("<5I", _DDSCAPS_TEXTURE, caps2, 0, 0, 0)
    hdr += struct.pack("<5I", dxgi, 4 if is3d else 3, 0, 1, 0)
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(data.tobytes())


def read_dds(path):
    """(dxgi, data) を返す。write_dds の逆。"""
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != _MAGIC:
        raise ValueError("DDS ではない")
    (size, flags, h, w, _pitch, depth, mips) = struct.unpack_from("<7I", raw, 4)
    if size != 124 or mips != 1:
        raise ValueError("想定外のヘッダ")
    fourcc = struct.unpack_from("<I", raw, 4 + 7 * 4 + 44 + 8)[0]
    if fourcc != int.from_bytes(b"DX10", "little"):
        raise ValueError("DX10 拡張ヘッダが無い")
    dxgi, dim = struct.unpack_from("<2I", raw, 128)
    comps, _bpe, dt = _FORMATS[dxgi]
    arr = np.frombuffer(raw, dtype=dt, offset=148)
    if dim == 4:
        return dxgi, arr.reshape(depth, h, w, comps)
    return dxgi, arr.reshape(h, w, comps)
