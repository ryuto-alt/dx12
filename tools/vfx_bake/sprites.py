"""火花(sparks)と火の粉(embers)のスプライト・アトラス、カール場(curl64)。

炎と同じ RGBA の規約(fire family): R = 温度(0 冷 .. 1 最高。黒体の色の引き表に使う)、G = 不透明度、
B = 浸食(α エロージョンの閾値の場。小さいところから削れる)、A = 自己発光の重み。
枠は「寿命」の順(0 = 生まれたて .. 最後 = 消える直前)。
"""
import numpy as np

import noise


def _grid(n):
    y, x = np.mgrid[0:n, 0:n].astype(np.float32)
    return (x + 0.5) / n * 2 - 1, (y + 0.5) / n * 2 - 1  # -1..1。y は下向き


def _smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def sparks(frames=8, res=64, seed=21):
    """火花の筋。+u(右)が進行方向の頭。寿命とともに筋が縮み、温度が落ちる。"""
    lats = [noise.make_lattice(seed + i) for i in range(2)]
    rng = np.random.default_rng(seed)
    x, y = _grid(res)
    out = []
    for i in range(frames):
        life = i / max(frames - 1, 1)
        length = 0.92 - 0.55 * life          # 筋の長さ(-1..1 の半幅の割合)
        wob = (rng.random() - 0.5) * 0.06    # 枠ごとの小さな曲がり
        yy = y - wob * (x + 1.0) ** 2
        width = 0.075 + 0.02 * (1 - life)
        s = (x + 1.0) / 2.0                  # 0 尾 .. 1 頭
        head = 0.5 + 0.5 * length            # 頭の位置(u)
        t = np.clip((x - (head * 2 - 1 - length * 2)) / (length * 2 + 1e-4), 0, 1)  # 筋の中の 0 尾 .. 1 頭
        inside = (x <= head * 2 - 1 + 0.02) & (t >= 0)
        prof = np.exp(-(yy / (width * (0.35 + 0.65 * t))) ** 2)
        body = prof * inside * _smooth(0.0, 0.25, t) * (1 - _smooth(0.96, 1.0, t))
        glow = np.exp(-(((x - (head * 2 - 1)) / 0.14) ** 2 + (yy / 0.14) ** 2)) * (1 - life * 0.6)
        a = np.clip(body + 0.45 * glow, 0, 1)
        temp = np.clip((0.25 + 0.75 * t) * (1 - 0.7 * life), 0, 1) * (a > 0.01)
        n = noise.fbm3(lats, (x + 1) * 3.0, (yy + 1) * 3.0, np.full_like(x, i * 0.37), base=1.0)
        eros = np.clip(0.55 * (1 - t) + 0.45 * n, 0, 1) * (a > 0.01)
        emis = np.clip(a * (0.6 + 0.4 * t) * (1 - 0.5 * life), 0, 1)
        out.append(np.stack([temp, a, eros, emis], axis=-1))
    return np.stack(out).astype(np.float32)


def embers(frames=16, res=64, seed=33):
    """火の粉。不整形の熾(おき)。寿命とともに熱が引き、灰の殻が広がる。"""
    lats = [noise.make_lattice(seed + i) for i in range(3)]
    x, y = _grid(res)
    r = np.sqrt(x * x + y * y)
    ang = np.arctan2(y, x)
    out = []
    for i in range(frames):
        life = i / max(frames - 1, 1)
        # 角度で揺れる縁(不整形)。枠ごとに同じ形を保つ(角度のノイズは固定、年齢は第 3 軸)
        ex = np.cos(ang) * 1.5 + 8.0
        ey = np.sin(ang) * 1.5 + 8.0
        edge = noise.fbm3(lats, ex, ey, np.full_like(x, 0.5), base=1.0)
        radius = (0.62 - 0.14 * life) * (0.75 + 0.5 * edge)
        core = 1 - _smooth(radius * 0.55, radius, r)
        halo = np.exp(-(r / (0.5 + 0.1 * (1 - life))) ** 2) * (0.5 * (1 - life) ** 1.5)
        a = np.clip(core + halo, 0, 1)
        crust = noise.fbm3(lats, (x + 1) * 2.5, (y + 1) * 2.5, np.full_like(x, 2.0 + life * 1.5), base=1.0)
        heat = np.clip((1 - life) * (1.15 - 0.55 * crust) * core + halo * 0.5, 0, 1)
        temp = np.clip(heat, 0, 1) * (a > 0.01)
        eros = np.clip(0.5 * (1 - r / max(radius.max(), 1e-3)) + 0.5 * crust, 0, 1) * (a > 0.01)
        emis = np.clip(heat ** 1.2 * a, 0, 1)
        out.append(np.stack([temp, a, eros, emis], axis=-1))
    return np.stack(out).astype(np.float32)


def curl_field(n=64, seed=55, modes=40, kmax=4):
    """周期的な 3D カール場。ベクトルポテンシャル(周期フーリエ)の離散カール(中心差分)なので、
    同じ中心差分での発散は丸め以外で 0。戻り値 [n,n,n,4] float32(xyz = 速度。-1..1 に収める。w = スカラーの追加ノイズ)。
    配列の軸は [z, y, x](3D テクスチャの深さ、行、列)。"""
    rng = np.random.default_rng(seed)
    g = np.arange(n, dtype=np.float32) / n
    gz, gy, gx = np.meshgrid(g, g, g, indexing="ij")
    psi = np.zeros((3, n, n, n), np.float32)
    for c in range(3):
        for _ in range(modes):
            k = rng.integers(-kmax, kmax + 1, size=3)
            if not k.any():
                continue
            amp = 1.0 / (np.linalg.norm(k) ** 1.2)
            ph = rng.random() * 2 * np.pi
            psi[c] += np.float32(amp) * np.cos(2 * np.pi * (k[0] * gx + k[1] * gy + k[2] * gz) + ph)

    def d(a, axis):  # 周期の中心差分(格子 1 個ぶんを 1 とする)
        return 0.5 * (np.roll(a, -1, axis) - np.roll(a, 1, axis))
    # 配列軸: 0=z, 1=y, 2=x。ψ の成分: 0=x, 1=y, 2=z
    ux = d(psi[2], 1) - d(psi[1], 0)  # ∂ψz/∂y − ∂ψy/∂z
    uy = d(psi[0], 0) - d(psi[2], 2)  # ∂ψx/∂z − ∂ψz/∂x
    uz = d(psi[1], 2) - d(psi[0], 1)  # ∂ψy/∂x − ∂ψx/∂y
    vel = np.stack([ux, uy, uz], axis=-1)
    vel = vel / np.abs(vel).max() * 0.95  # ピーク正規化(クリップしない。SNORM で 1.0 に張り付かない余裕)
    sc = noise.fbm3([noise.make_lattice(seed + 9 + i, 16) for i in range(2)], gx * 16, gy * 16, gz * 16)
    w = (sc - 0.5) * 2.0
    return np.concatenate([vel, w[..., None]], axis=-1).astype(np.float32)
