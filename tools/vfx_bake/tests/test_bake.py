"""VFX-02a 焼き込みの検査: DDS の往復、sha の安定、6-way の向き、MV の補間精度(+3dB)、カール場の発散、コミット済み資産の整合。"""
import hashlib
import json
import os
import time

import numpy as np
import pytest

import bake
import ddsio
import flipbook as fb
import sprites

ASSET_DIR = bake.DEFAULT_OUT


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse < 1e-12 else 10 * np.log10(1.0 / mse)


STRIDE = 4  # フリップブックの枠の間隔(= flipbook.Settings.spf)。真ん中の正解は 1 ステップ刻みの系列から取る


def premul(t, kind):
    a = t[..., 1:2] if kind == "fire" else t[..., 3:4]
    if kind == "fire":  # 温度 / 浸食を不透明度で重み付け。発光はそのまま
        return np.concatenate([t[..., 0:1] * a, a, t[..., 2:3] * a, t[..., 3:4]], -1)
    return np.concatenate([t[..., :3] * a, a], -1)


def divergence_ratio(u):
    """u: [z][y][x][3](xyz)。中心差分の発散の平均 / 勾配の大きさの平均。"""
    d = 0.5 * ((np.roll(u[..., 0], -1, 2) - np.roll(u[..., 0], 1, 2)) +
               (np.roll(u[..., 1], -1, 1) - np.roll(u[..., 1], 1, 1)) +
               (np.roll(u[..., 2], -1, 0) - np.roll(u[..., 2], 1, 0)))
    gm = np.sqrt(sum((0.5 * (np.roll(u[..., c], -1, ax) - np.roll(u[..., c], 1, ax))) ** 2
                     for c, ax in ((0, 2), (1, 1), (2, 0))))
    return float(np.abs(d).mean() / gm.mean())


# ---------------------------------------------------------------- DDS
def test_dds_roundtrip_2d_3d(tmp_path):
    rng = np.random.default_rng(1)
    a = rng.integers(0, 255, (6, 10, 4), dtype=np.uint8)
    ddsio.write_dds(tmp_path / "a.dds", ddsio.DXGI_R8G8B8A8_UNORM, a)
    f, b = ddsio.read_dds(tmp_path / "a.dds")
    assert f == ddsio.DXGI_R8G8B8A8_UNORM and np.array_equal(a, b)
    v = ddsio.snorm16(rng.uniform(-1, 1, (3, 5, 7, 4)))
    ddsio.write_dds(tmp_path / "v.dds", ddsio.DXGI_R16G16B16A16_SNORM, v)
    f, w = ddsio.read_dds(tmp_path / "v.dds")
    assert f == ddsio.DXGI_R16G16B16A16_SNORM and np.array_equal(v, w)
    raw = (tmp_path / "v.dds").read_bytes()
    assert raw[:4] == b"DDS " and len(raw) == 148 + v.nbytes


# ---------------------------------------------------------------- 決定論(sha の安定)
def test_bake_sha_stable(tmp_path):
    m1 = bake.run(str(tmp_path / "a"), True, bake.ALL)
    m2 = bake.run(str(tmp_path / "b"), True, bake.ALL)
    s1 = {k: v["sha256"] for k, v in m1["files"].items()}
    s2 = {k: v["sha256"] for k, v in m2["files"].items()}
    assert s1 == s2 and len(s1) == 8
    assert sha(tmp_path / "a" / "vfx_bake_manifest.json") == sha(tmp_path / "b" / "vfx_bake_manifest.json")
    for k, v in m1["files"].items():
        assert sha(tmp_path / "a" / k) == v["sha256"]


def test_quick_under_60s(tmp_path):
    t0 = time.time()
    bake.run(str(tmp_path), True, bake.ALL)
    assert time.time() - t0 < 60


# ---------------------------------------------------------------- 6-way の向き
def test_six_way_axes(quick_seq):
    s = quick_seq["smoke"]
    rlu, dbf = s["tex"], s["tex2"]
    a = rlu[..., 3]
    assert a.mean() > 0.01
    H, W = a.shape[1:3]
    xs = np.arange(W)[None, None, :]
    ys = np.arange(H)[None, :, None]  # 行は下向き

    def cx(v):  # 不透明度で重み付けた明るさの重心
        w = v * a
        return (w * xs).sum() / w.sum()

    def cy(v):
        w = v * a
        return (w * ys).sum() / w.sum()
    # 右から来る光は右側の面を明るくする(重心が右)
    assert cx(rlu[..., 0]) > cx(rlu[..., 1]) + 0.1
    # 上から来る光は上側が明るい(行は下向きなので重心の行が小さい)
    assert cy(rlu[..., 2]) < cy(dbf[..., 0]) - 0.1
    # カメラ側(手前)から来る光は、奥からより明るい
    assert (dbf[..., 2] * a).sum() > (dbf[..., 1] * a).sum() * 1.02
    # 参照の合成式: 軸に沿った光は、その側の値をそのまま返す
    px_rlu, px_dbf = rlu[0, 30, 20], dbf[0, 30, 20]
    assert fb.six_way_light([1, 0, 0], px_rlu, px_dbf) == pytest.approx(px_rlu[0])
    assert fb.six_way_light([-1, 0, 0], px_rlu, px_dbf) == pytest.approx(px_rlu[1])
    assert fb.six_way_light([0, 1, 0], px_rlu, px_dbf) == pytest.approx(px_rlu[2])
    assert fb.six_way_light([0, -1, 0], px_rlu, px_dbf) == pytest.approx(px_dbf[0])
    assert fb.six_way_light([0, 0, 1], px_rlu, px_dbf) == pytest.approx(px_dbf[2])
    assert fb.six_way_light([0, 0, -1], px_rlu, px_dbf) == pytest.approx(px_dbf[1])


# ---------------------------------------------------------------- MV の補間精度
@pytest.mark.parametrize("kind", ["fire", "smoke"])
def test_mv_blend_psnr_gain(dense_seq, kind):
    """偶数枠だけをフリップブックとし、奇数枠(間の正解)を MV 補間とクロスフェードで再現して PSNR を比べる。
    MV は保存形式と同じ SNORM16 を通す(枠の間隔が STRIDE 倍なので MV も STRIDE 倍)。
    比べるのは、実際に合成される量(色 × 不透明度)。不透明度の低い画素の正規化された色はノイズで、見た目に効かない。"""
    _st, seqs = dense_seq
    s = seqs[kind]
    tex = s["tex"]
    mv = ddsio.snorm16(np.clip(s["mv"] * STRIDE / bake.MV_RANGE, -1, 1)).astype(np.float32) / 32767 * bake.MV_RANGE
    gains = []
    for k in range(0, len(tex) - STRIDE, STRIDE):
        truth = premul(tex[k + STRIDE // 2], kind)
        warp = premul(fb.blend_frames(tex[k], tex[k + STRIDE], mv[k], mv[k + STRIDE], 0.5), kind)
        cross = premul(fb.crossfade(tex[k], tex[k + STRIDE], 0.5), kind)
        gains.append(psnr(warp, truth) - psnr(cross, truth))
    assert np.mean(gains) >= 3.0, f"{kind}: PSNR 利得 {np.mean(gains):.2f}dB"
    assert min(gains) > 0.0


def test_mv_not_clipped(quick_seq):
    for k in ("fire", "smoke"):
        assert np.abs(quick_seq[k]["mv"]).max() < bake.MV_RANGE * 0.95


# ---------------------------------------------------------------- ループの継ぎ目
@pytest.mark.parametrize("kind", ["fire", "smoke"])
def test_loop_seam(quick_seq, kind):
    loop = fb.loop_blend(quick_seq[kind], fb.QUICK)["tex"]
    step = np.mean([np.abs(loop[i + 1] - loop[i]).mean() for i in range(len(loop) - 1)])
    seam = np.abs(loop[0] - loop[-1]).mean()
    worst = max(np.abs(loop[i + 1] - loop[i]).mean() for i in range(len(loop) - 1))
    assert seam <= worst * 1.25, (seam, step, worst)


# ---------------------------------------------------------------- カール場
def test_curl_divergence_and_periodicity():
    vol = sprites.curl_field(32)
    u = ddsio.snorm16(vol).astype(np.float32) / 32767
    assert divergence_ratio(u[..., :3]) < 0.02
    assert np.abs(vol[..., :3]).max() <= 0.96  # クリップ無し(ピーク正規化)
    assert vol[..., :3].std() > 0.15         # 十分な振幅
    # 周期的: 端どうしの差は内部の隣接差と同程度
    seam = np.abs(u[0] - u[-1]).mean()
    inner = np.abs(u[1:] - u[:-1]).mean()
    assert seam < inner * 1.5
    assert np.abs(vol[..., 3]).max() <= 1.0


# ---------------------------------------------------------------- スプライト
def test_sparks_embers_semantics():
    sp = sprites.sparks(8, 32)
    assert sp.shape == (8, 32, 32, 4) and sp.min() >= 0 and sp.max() <= 1
    w = sp[0, ..., 1]
    xs = np.arange(32)[None, :]
    assert (w * xs).sum() / w.sum() > 15.5  # 頭(右)に質量が寄る
    assert sp[0, ..., 0].max() > sp[-1, ..., 0].max()  # 熱が引く
    assert sp[0, ..., 1].sum() > sp[-1, ..., 1].sum()  # 筋が縮む
    em = sprites.embers(16, 32)
    assert em.shape == (16, 32, 32, 4) and em.min() >= 0 and em.max() <= 1
    assert em[0, ..., 3].sum() > em[-1, ..., 3].sum() * 1.5
    assert em[0, 16, 16, 1] > 0.5  # 中心は不透明


# ---------------------------------------------------------------- コミット済み資産
def _manifest():
    p = os.path.join(ASSET_DIR, "vfx_bake_manifest.json")
    if not os.path.exists(p):
        pytest.skip("資産が未生成")
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def test_committed_assets_match_manifest():
    m = _manifest()
    assert not m["quick"]
    names = set(m["files"])
    assert {"fire_flipbook.dds", "fire_mv.dds", "smoke_6way_rlu.dds", "smoke_6way_dbf.dds", "smoke_mv.dds",
            "sparks.dds", "embers.dds", "curl64.dds"} <= names
    total = 0
    for name, e in m["files"].items():
        p = os.path.join(ASSET_DIR, name)
        assert sha(p) == e["sha256"], name
        assert os.path.getsize(p) == e["bytes"]
        dxgi, arr = ddsio.read_dds(p)
        assert dxgi == e["dxgi"] and arr.shape[-2] == e["width"] and arr.shape[-3] == e["height"]
        total += e["bytes"]
    assert total < 16 * 1024 * 1024  # 資産のサイズの予算
    assert os.path.exists(os.path.join(ASSET_DIR, "LICENSE.md"))


def test_committed_flipbooks_are_sane():
    m = _manifest()
    for name in ("fire_flipbook.dds", "smoke_6way_rlu.dds"):
        e = m["files"][name]
        _d, a = ddsio.read_dds(os.path.join(ASSET_DIR, name))
        fr = fb.unpack_atlas(a.astype(np.float32) / 255.0, e["cols"], e["rows"])
        alpha = fr[..., 1 if name.startswith("fire") else 3]
        cover = (alpha > 0.1).mean(axis=(1, 2))
        assert cover.min() > 0.01, f"{name}: 空の枠がある"
        assert cover.max() < 0.9
        assert alpha[:, :, 0].max() < 0.05 and alpha[:, :, -1].max() < 0.05  # 左右が切れていない
        assert alpha[:, 0, :].max() < 0.05  # 上端は空(根元固定なので下端だけ残る)
        assert alpha[:, -1, :].max() < 0.9, name  # 下端の残りは manifest の anchoring に書いた範囲
    assert "anchoring" in m["conventions"]
    for name in ("fire_mv.dds", "smoke_mv.dds"):
        _d, a = ddsio.read_dds(os.path.join(ASSET_DIR, name))
        assert np.abs(a).max() < 32767 * 0.95  # クリップ無し
        assert np.abs(a).mean() > 20  # 動きがある


def test_committed_curl_divergence():
    m = _manifest()
    _d, a = ddsio.read_dds(os.path.join(ASSET_DIR, "curl64.dds"))
    assert a.shape == (64, 64, 64, 4)
    assert divergence_ratio(a[..., :3].astype(np.float32) / 32767) < 0.02
    assert m["files"]["curl64.dds"]["depth"] == 64


@pytest.mark.parametrize("kind", ["fire", "smoke"])
def test_mv_blend_gain_heldout(kind):
    """調整に使っていない系列(別の乱数シード・別の格子 48)でも PSNR 利得が 3dB 以上。"""
    st = fb.Settings(grid=48, res=64, mv_res=32, cols=4, rows=3, spf=1, overlap=3, warmup=100, seed=23)
    s = fb.bake_sequence(kind, st, frames=17)
    tex = s["tex"]
    mv = ddsio.snorm16(np.clip(s["mv"] * STRIDE / bake.MV_RANGE, -1, 1)).astype(np.float32) / 32767 * bake.MV_RANGE
    gains = []
    for k in range(0, len(tex) - STRIDE, STRIDE):
        truth = premul(tex[k + STRIDE // 2], kind)
        warp = premul(fb.blend_frames(tex[k], tex[k + STRIDE], mv[k], mv[k + STRIDE], 0.5), kind)
        cross = premul(fb.crossfade(tex[k], tex[k + STRIDE], 0.5), kind)
        gains.append(psnr(warp, truth) - psnr(cross, truth))
    assert np.mean(gains) >= 3.0, f"{kind}: 保持外の系列の PSNR 利得 {np.mean(gains):.2f}dB"
