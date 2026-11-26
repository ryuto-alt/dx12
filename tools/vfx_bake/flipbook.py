"""炎と煙のフリップブック焼き込み(流体シム → 正射影のレイマーチ → アトラス + モーションベクトル + 6-way)。

規約(BD7 のシェーダはこれに合わせる。vfx_bake_manifest.json にも同じ内容を書く):
  アトラスの枠の並びは左上から行優先(frame = row * cols + col)。枠の中の uv は左上が (0,0)、v は下向き。
  ビルボードのローカル軸: +x = 画面右、+y = 画面上、+z = カメラ側。6-way の名前は「光が来る側」。
  MV は R16G16_SNORM。変位 d = snorm * mvRange(枠の uv 単位。u は右向き、v は下向き)で、
  「フレーム i の絵が次のフレーム i+1 までに動く量」。最後のフレームは 0 番へ(ループ)。
"""
from dataclasses import dataclass

import numpy as np

import noise
from fluid import Fluid3D


@dataclass(frozen=True)
class Settings:
    grid: int = 64        # シムの格子(1 辺)
    res: int = 128        # 枠の解像度(色)
    mv_res: int = 64      # 枠の解像度(MV)
    cols: int = 6
    rows: int = 6
    spf: int = 4          # 1 枠あたりのシムのステップ数(枠の間隔。動きが小さすぎると MV の利得が出ない)
    overlap: int = 9      # ループの継ぎ目を混ぜる枠数
    warmup: int = 70      # 定常状態まで回すステップ
    seed: int = 7

    @property
    def frames(self):
        return self.cols * self.rows


# 奥行きで平均した気体の速度は、見かけの絵の動きより大きい(半ラグランジュの拡散と、手前ほど遅く見える効果)。
# 補間誤差(PSNR)が最小になる倍率を 0.55〜1.0 で測ると 0.65〜0.75 が最良(炎・煙とも)。中央の 0.7 を使う。
MV_GAIN = 0.7

FULL = Settings()
QUICK = Settings(grid=32, res=64, mv_res=32, cols=4, rows=3, spf=4, overlap=3, warmup=36)


# ---------------------------------------------------------------- シム
FIRE = dict(vfade=0.1, top0=0.78, fade_rate=0.12, side0=0.8, drag=0.07, vin=1.6, buoy_T=0.19, buoy_D=0.0, cool=0.021, d_decay=0.020, vort=0.9, src_r=0.125, jitter=1.2,
            src_T=1.0, src_D=1.0)
SMOKE = dict(vfade=0.2, top0=0.62, fade_rate=0.45, side0=0.5, drag=0.035, vin=1.2, buoy_T=0.09, buoy_D=0.004, cool=0.012, d_decay=0.035, vort=0.6, src_r=0.115, jitter=0.4,
             src_T=0.9, src_D=1.0)


def _edge_fade(sim, side0=0.8, top0=0.78):
    n = sim.n
    g = sim.gy / (n - 1)
    top = np.clip((g - top0) / (1.0 - top0), 0, 1)
    sx = np.abs(sim.gx / (n - 1) - 0.5) * 2
    sz = np.abs(sim.gz / (n - 1) - 0.5) * 2
    side = np.clip((np.maximum(sx, sz) - side0) / (1.0 - side0), 0, 1)
    return np.maximum(top, side).astype(np.float32)


def run_sim(kind, st, total_frames):
    """warmup の後、spf 刻みで total_frames 回、シムの状態(Fluid3D。すぐ使うこと)を返すジェネレータ。"""
    P = FIRE if kind == "fire" else SMOKE
    sim = Fluid3D(st.grid)
    n = st.grid
    rng = np.random.default_rng(st.seed + (0 if kind == "fire" else 1000))
    cx = cz = (n - 1) / 2
    fade = _edge_fade(sim, P["side0"], P["top0"])
    scale = n / 64.0  # 速度などは 64 を基準に格子へ換算(格子が変わってもシムの絵が変わりにくいように)
    dt = 1.0
    jx = jz = 0.0   # 横風と脈動は白色ではなく自己相関のある乱数(渦巻き・ちぎれが出る)
    amp = 1.0
    step = 0
    nsteps = st.warmup + total_frames * st.spf
    while step < nsteps:
        radius = max(2.0, P["src_r"] * n * (1.0 + 0.25 * (rng.random() - 0.5)))
        m = sim.disk_mask(cx + (rng.random() - 0.5) * 0.04 * n, cz + (rng.random() - 0.5) * 0.04 * n,
                          radius, 0.0, max(2.0, 0.05 * n))
        jx = 0.85 * jx + 0.15 * (rng.random() - 0.5) * 2
        jz = 0.85 * jz + 0.15 * (rng.random() - 0.5) * 2
        amp = float(np.clip(0.8 * amp + 0.2 * (0.35 + 1.3 * rng.random()), 0.3, 1.4))
        m = m * np.float32(min(amp, 1.0))
        D, T, age, ox, oz = sim["D"], sim["T"], sim["age"], sim["ox"], sim["oz"]
        np.maximum(D, m * P["src_D"], out=D)
        np.maximum(T, m * P["src_T"], out=T)
        sim["v"][:] = np.maximum(sim["v"], m * P["vin"] * scale)
        sim["u"][:] += m * jx * P["jitter"] * scale
        sim["w"][:] += m * jz * P["jitter"] * scale
        sim["v"][:] += m * (amp - 0.8) * 0.8 * scale
        fresh = m > 0.15
        age[fresh] = 0
        ox[fresh] = sim.gx[fresh]
        oz[fresh] = sim.gz[fresh]
        # 力
        sim["v"][:] += dt * (P["buoy_T"] * T - P["buoy_D"] * D) * scale
        sim.vorticity_confinement(P["vort"], dt)
        sim.project(30)
        sim.advect(dt)
        sim.project(30)
        sim["T"][:] *= np.float32(np.exp(-P["cool"] * dt))
        if kind == "fire":
            sim["T"][:] = np.maximum(sim["T"] - 0.0025 * dt, 0)
        sim["D"][:] *= np.float32(np.exp(-P["d_decay"] * dt)) * (1.0 - P["fade_rate"] * fade)
        sim["T"][:] *= (1.0 - P["fade_rate"] * fade)
        sim["age"][:] += dt
        for c in ("u", "v", "w"):  # 古い渦が溜まらないように少し減衰
            sim[c][:] *= np.float32(1.0 - P["drag"]) * (1.0 - P["vfade"] * fade)
        step += 1
        if step > st.warmup and (step - st.warmup - 1) % st.spf == st.spf - 1:
            yield sim


# ---------------------------------------------------------------- 描画(正射影のレイマーチ)
def _zoom_axis(a, axis, m):
    n = a.shape[axis]
    if m == n:
        return a
    pos = np.clip((np.arange(m, dtype=np.float32) + 0.5) * n / m - 0.5, 0, n - 1)
    i0 = np.minimum(np.floor(pos).astype(np.int64), n - 2)
    f = (pos - i0).astype(np.float32)
    shp = [1] * a.ndim
    shp[axis] = m
    f = f.reshape(shp)
    return np.take(a, i0, axis=axis) * (1 - f) + np.take(a, i0 + 1, axis=axis) * f


def upsample_xy(a, m):
    return _zoom_axis(_zoom_axis(a, 0, m), 1, m)


def to_image(a):
    """a[x, y, ...] → 画像 [row(上が y 大), col(x)]"""
    return np.ascontiguousarray(np.swapaxes(a, 0, 1)[::-1])


class Detail:
    """シムの 1 格子より細かい模様を、流れに乗る属性(出生位置と年齢)から作る。MV と整合する。"""

    def __init__(self, seed):
        self.lats = [noise.make_lattice(seed + i) for i in range(3)]

    def __call__(self, ox, oz, age, y):
        return noise.fbm3(self.lats, ox * 0.30, age * 0.02, oz * 0.30, base=1.0)


def _volume(sim, st, detail, kind):
    """描画解像度の (R,R,Z) の体積。z は手前(+z)が先頭。"""
    R = st.res
    n = st.grid

    def up(a):
        return upsample_xy(a, R)[:, :, ::-1]
    D, T, age, ox, oz, u, v = (up(sim[k]) for k in ("D", "T", "age", "ox", "oz", "u", "v"))
    yy = (np.arange(R, dtype=np.float32) + 0.5) * n / R
    ygrid = np.broadcast_to(yy[None, :, None], D.shape)
    d = detail(ox, oz, age, ygrid)
    if kind == "fire":
        sig = D * np.clip((T - 0.12) / 0.5, 0, 1) ** 1.3
        sig = sig * np.clip(0.45 + 1.9 * (d - 0.30) * (1.2 - T * 0.5), 0, 1.6)
    else:
        sig = np.maximum(D, 0) ** 1.15 * np.clip(0.30 + 1.45 * d, 0, 1.8)
    return sig.astype(np.float32), T, age, u, v, d


def _composite(sig, kappa):
    """前から奥へ。slab の寄与 w、前方の透過率 Tv(排他)、アルファ和を返す。z は軸 2。"""
    tau = np.cumsum(sig, axis=2) * kappa
    tau_ex = tau - sig * kappa
    Tv = np.exp(-tau_ex)
    w = Tv * (1.0 - np.exp(-sig * kappa))
    return w, Tv, w.sum(axis=2)


def _wavg(w, a, wsum):
    return (w * a).sum(axis=2) / np.maximum(wsum, 1e-6)


def _yfrac(shape):
    """枠の下端 0 .. 上端 1 の高さ(体積の形)。"""
    return np.broadcast_to((np.arange(shape[1], dtype=np.float32) + 0.5)[None, :, None] / shape[1], shape)


def render_fire(sim, st, detail):
    sig, T, age, u, v, d = _volume(sim, st, detail, "fire")
    kappa = 5.0 / st.grid
    w, _Tv, a = _composite(sig, kappa)
    yfrac = _yfrac(sig.shape)
    temp = _wavg(w, T, a)
    emis = 1.0 - np.exp(-(sig * (0.3 + T) ** 2).sum(axis=2) * kappa * 1.7)
    eros = _wavg(w, np.clip(0.3 * (1 - yfrac) + 0.7 * d, 0, 1), a)
    rgba = np.stack([np.clip(temp, 0, 1), np.clip(a, 0, 1), eros, np.clip(emis, 0, 1)], axis=-1)
    vel = np.stack([_wavg(w, u, a), -_wavg(w, v, a)], axis=-1) * st.spf / st.grid  # uv/枠。v は下向き
    return to_image(rgba).astype(np.float32), to_image(vel).astype(np.float32)


SIX = ("right", "left", "top", "bottom", "back", "front")


def _lit(sig, kappa, side):
    """side から光が来るときに各セルへ届く割合(減衰 + 小さな環境項)。体積は (x, y, z手前先頭)。"""
    if side == "right":
        tau = np.cumsum(sig[::-1], axis=0)[::-1] - sig
    elif side == "left":
        tau = np.cumsum(sig, axis=0) - sig
    elif side == "top":
        tau = np.cumsum(sig[:, ::-1], axis=1)[:, ::-1] - sig
    elif side == "bottom":
        tau = np.cumsum(sig, axis=1) - sig
    elif side == "front":  # カメラ側から(z=0 が手前)
        tau = np.cumsum(sig, axis=2) - sig
    else:  # back
        tau = np.cumsum(sig[:, :, ::-1], axis=2)[:, :, ::-1] - sig
    return 0.04 + 0.96 * np.exp(-tau * kappa * 0.8)


def render_smoke(sim, st, detail):
    sig, T, age, u, v, d = _volume(sim, st, detail, "smoke")
    kappa = 9.0 / st.grid
    w, _Tv, a = _composite(sig, kappa)
    yfrac = _yfrac(sig.shape)
    lit = {s: _wavg(w, _lit(sig, kappa, s), a) for s in SIX}
    eros = _wavg(w, np.clip(0.3 * (1 - yfrac) + 0.7 * d, 0, 1), a)
    rlu = np.stack([lit["right"], lit["left"], lit["top"], np.clip(a, 0, 1)], axis=-1)
    dbf = np.stack([lit["bottom"], lit["back"], lit["front"], eros], axis=-1)
    vel = np.stack([_wavg(w, u, a), -_wavg(w, v, a)], axis=-1) * st.spf / st.grid
    return to_image(rlu).astype(np.float32), to_image(dbf).astype(np.float32), to_image(vel).astype(np.float32)


# ---------------------------------------------------------------- MV の整え
def _box(a):
    p = np.pad(a, ((1, 1), (1, 1)) + ((0, 0),) * (a.ndim - 2), mode="edge")
    acc = 0
    for dy in range(3):
        for dx in range(3):
            acc = acc + p[dy:dy + a.shape[0], dx:dx + a.shape[1]]
    return acc / 9.0


def finish_mv(vel, alpha, st, iters=10):
    """MV を mv_res へ縮め、輪郭の外へ押し広げる(補間で輪郭外の画素が引く先にも MV が要る)。"""
    f = st.res // st.mv_res
    h = vel.shape[0] // f
    wgt = alpha[..., None]
    vs = (vel * wgt).reshape(h, f, h, f, 2).sum(axis=(1, 3))
    ws = wgt.reshape(h, f, h, f, 1).sum(axis=(1, 3))
    valid = ws[..., 0] > 1e-4
    mv = np.where(valid[..., None], vs / np.maximum(ws, 1e-6), 0.0)
    for _ in range(iters):
        num = _box(mv * valid[..., None])
        den = _box(valid.astype(np.float32))[..., None]
        grown = num / np.maximum(den, 1e-6)
        mv = np.where(valid[..., None], mv, grown)
        valid = valid | (den[..., 0] > 1e-4)
    return mv.astype(np.float32)


# ---------------------------------------------------------------- 全体の焼き込み
def bake_sequence(kind, st, frames=None):
    """シム → 描画。frames 枠(既定は st.frames + st.overlap)の生の系列を返す。
    fire: {'tex','mv'}、smoke: {'tex'(RLU),'tex2'(DBF),'mv'}。"""
    frames = frames if frames is not None else st.frames + st.overlap
    detail = Detail(st.seed + 31)
    tex, tex2, mvs = [], [], []
    for sim in run_sim(kind, st, frames):
        if kind == "fire":
            rgba, vel = render_fire(sim, st, detail)
            tex.append(rgba)
            alpha = rgba[..., 1]
        else:
            rlu, dbf, vel = render_smoke(sim, st, detail)
            tex.append(rlu)
            tex2.append(dbf)
            alpha = rlu[..., 3]
        mvs.append(finish_mv(vel * MV_GAIN, alpha, st))
    out = {"tex": np.stack(tex), "mv": np.stack(mvs)}
    if tex2:
        out["tex2"] = np.stack(tex2)
    return out


def loop_blend(seq, st):
    """継ぎ目を目立たなくする: 先頭 overlap 枠を、末尾の続き(F 以降)と混ぜる。"""
    F, O = st.frames, st.overlap
    res = {}
    for k, a in seq.items():
        loop = a[:F].copy()
        for j in range(O):
            t = (j + 1) / (O + 1)  # 先頭に近いほど続きの側
            loop[j] = a[F + j] * (1 - t) + a[j] * t
        res[k] = loop
    return res


def pack_atlas(frames, cols, rows):
    """[F,h,w,C] → [rows*h, cols*w, C]。frame = row*cols + col。"""
    F, h, w, C = frames.shape
    assert F == cols * rows
    return frames.reshape(rows, cols, h, w, C).transpose(0, 2, 1, 3, 4).reshape(rows * h, cols * w, C)


def unpack_atlas(atlas, cols, rows):
    H, W, C = atlas.shape
    h, w = H // rows, W // cols
    return atlas.reshape(rows, h, cols, w, C).transpose(0, 2, 1, 3, 4).reshape(rows * cols, h, w, C)


# ---------------------------------------------------------------- 参照のブレンド(BD7 のシェーダの正)
def _bilinear(img, u, v):
    """img[h,w,C]、u,v は 0..1(枠内。外はクランプ)。"""
    h, w = img.shape[:2]
    x = np.clip(u * w - 0.5, 0, w - 1)
    y = np.clip(v * h - 0.5, 0, h - 1)
    x0 = np.minimum(np.floor(x).astype(int), max(w - 2, 0))
    y0 = np.minimum(np.floor(y).astype(int), max(h - 2, 0))
    fx = (x - x0)[..., None]
    fy = (y - y0)[..., None]
    x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    return (img[y0, x0] * (1 - fx) * (1 - fy) + img[y0, x1] * fx * (1 - fy) +
            img[y1, x0] * (1 - fx) * fy + img[y1, x1] * fx * fy)


def blend_frames(frameA, frameB, mvA, mvB, t):
    """フレーム A→B の間の t(0..1)を MV で補間する(Lozar 方式)。
    A は自分の MV で前へ t だけ、B は自分の MV で後ろへ (1-t) だけずらして引き、t で混ぜる(MV は引く先で評価、2 回反復)。
    frame*: [h,w,C]、mv*: [h',w',2](uv/枠。A の動きは A→次、B の動きは B→次)。"""
    h, w = frameA.shape[:2]
    v, u = np.mgrid[0:h, 0:w].astype(np.float32)
    u = (u + 0.5) / w
    v = (v + 0.5) / h
    # MV は「絵の動き」の場なので、引く先の位置で評価し直す(2 回の固定点反復)。出力の位置の MV で引くと動きが大きい所でずれる
    uA, vA, uB, vB = u, v, u, v
    for _ in range(2):
        dA = _bilinear(mvA, uA, vA)
        dB = _bilinear(mvB, uB, vB)
        uA, vA = u - dA[..., 0] * t, v - dA[..., 1] * t
        uB, vB = u + dB[..., 0] * (1 - t), v + dB[..., 1] * (1 - t)
    a = _bilinear(frameA, uA, vA)
    b = _bilinear(frameB, uB, vB)
    return a * (1 - t) + b * t


def crossfade(frameA, frameB, t):
    return frameA * (1 - t) + frameB * t


def six_way_light(L, rlu, dbf):
    """L: 影を付ける点から光へ向かう単位ベクトル(ビルボード空間: x 右、y 上、z カメラ側)。
    rlu/dbf: 同じ画素の [..,4]。戻り値は光の量(アルファを掛ける前)。"""
    L = np.asarray(L, np.float32)
    w = L * L
    cx = rlu[..., 0] if L[0] > 0 else rlu[..., 1]
    cy = rlu[..., 2] if L[1] > 0 else dbf[..., 0]
    cz = dbf[..., 2] if L[2] > 0 else dbf[..., 1]
    return w[0] * cx + w[1] * cy + w[2] * cz
