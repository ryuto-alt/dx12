"""周期的な値ノイズ(3D)と fbm。乱数は固定シードの numpy Generator だけ。"""
import numpy as np


def make_lattice(seed, size=32):
    return np.random.default_rng(seed).random((size, size, size), dtype=np.float32)


def _smooth(t):
    return t * t * (3 - 2 * t)


def value_noise3(lat, x, y, z):
    """lat: (L,L,L)、x,y,z: 格子単位の座標(同じ形の配列)。L で周期的。0..1。"""
    L = lat.shape[0]
    xi, yi, zi = (np.floor(a).astype(np.int64) for a in (x, y, z))
    fx, fy, fz = (_smooth((a - np.floor(a)).astype(np.float32)) for a in (x, y, z))
    out = 0.0
    for dx in (0, 1):
        wx = fx if dx else 1 - fx
        for dy in (0, 1):
            wy = fy if dy else 1 - fy
            for dz in (0, 1):
                wz = fz if dz else 1 - fz
                out = out + lat[(xi + dx) % L, (yi + dy) % L, (zi + dz) % L] * (wx * wy * wz)
    return out


def fbm3(lats, x, y, z, base=1.0, gain=0.5, lac=2.0):
    """lats: 倍音ごとの格子のリスト。0..1 に正規化。"""
    acc, amp, tot, f = 0.0, 1.0, 0.0, base
    for lat in lats:
        acc = acc + amp * value_noise3(lat, x * f, y * f, z * f)
        tot += amp
        amp *= gain
        f *= lac
    return acc / tot
