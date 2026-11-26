#!/usr/bin/env python3
"""VFX-02a 焼き込み: 炎・煙(6-way + MV)・火花・火の粉・カール場 → assets/engine/vfx/。

使い方:
  python tools/vfx_bake/bake.py                 # 全量(数分)。assets/engine/vfx/ へ書く
  python tools/vfx_bake/bake.py --quick         # 縮小版(60 秒以内)。既定の出力先は一時フォルダ
  python tools/vfx_bake/bake.py --out DIR       # 出力先を変える
  python tools/vfx_bake/bake.py --only fire,curl
  python tools/vfx_bake/bake.py --verify        # 全量を一時フォルダへ焼き、コミット済みの sha256 と照合
  python tools/vfx_bake/bake.py --preview DIR   # 目視用の PNG も出す(コミットしない)

自作・CC0。乱数は固定シードで、同じ環境(numpy の版)では毎回同じ sha256 になる。numpy の版や CPU の SIMD が違うと
浮動小数の末尾がずれて sha が変わりうるので、資産の真正性はコミット済みの manifest の sha256 で見る。
"""
import argparse
import hashlib
import json
import os
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import ddsio  # noqa: E402
import flipbook as fb  # noqa: E402
import sprites  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_OUT = os.path.join(REPO, "assets", "engine", "vfx")
GENERATOR_VERSION = 1
FPS = 12  # 想定の再生速度(36 枠で 3 秒のループ。MV 補間で滑らかに見える。ゲーム側が変えてよい)
MV_RANGE = 0.5  # MV の SNORM 1.0 が表す枠 uv の変位

ALL = ("fire", "smoke", "sparks", "embers", "curl")

CONVENTIONS = {
    "atlasLayout": "枠は左上から行優先(frame = row*cols + col)。枠内の uv は左上が (0,0)、v は下向き。ミップ無し。",
    "dataTextures": "全テクスチャはリニアのデータ(sRGB ではない)・非圧縮。SRV の形式は dxgi のとおり。",
    "billboardAxes": "+x = 画面右、+y = 画面上、+z = カメラ側。",
    "fireFamilyChannels": "R = 温度(0..1。黒体の引き表へ)、G = 不透明度、B = 浸食場(小さいほど先に削れる)、A = 自己発光の重み。",
    "smoke6way": "RLU: R = 右から来る光、G = 左から、B = 上から、A = 不透明度。DBF: R = 下から、G = 奥(背面)から、B = 手前(カメラ側)から、A = 浸食場。"
                 "値は不透明度で正規化済みの光の量(使うときに A を掛ける)。ライト方向 L(ビルボード空間の単位ベクトル。表面から光へ)なら "
                 "光 = Lx²*(Lx>0?R:G of RLU) + Ly²*(Ly>0?B of RLU:R of DBF) + Lz²*(Lz>0?B:G of DBF)。",
    "motionVectors": "R16G16_SNORM。変位 d(枠 uv、u 右・v 下)= snorm * mvRange。フレーム i の絵が i+1 までに動く量。最後の枠は 0 番へ(ループ)。"
                     "補間(t は 0..1): A を uv - d_A(uv)*t から、B を uv + d_B(uv)*(1-t) から引き、t で混ぜる。"
                     "d は出力 uv で 1 回ではなく、引く位置で評価し直す(2 回の不動点反復: p=uv; 2 回 p = uv - d_A(p)*t、B も同様 uv + d_B(p)*(1-t))。"
                     "引く uv は枠の矩形に半テクセル内側でクランプしてから、アトラスのオフセットを足す(隣の枠を引かない)。MV は色より低解像度(同じ枠 uv で引く)。",
    "anchoring": "炎と煙は地面(枠の下端)に根元がある『根元固定』の枠で、下端の行に不透明度が残る(炎は最大約 0.6、煙は約 0.85)。"
                 "地面・ソケット・発生面に置くこと。宙に浮くビルボードに使う場合は、下端をフェードさせるか枠を上へずらして使う。",
    "curl": "RGBA16 SNORM の 64³、軸は [z][y][x]、xyz が速度(周期、ラップでタイル)、w は追加のスカラーノイズ。xyz は snorm * curlScale の意味ではなく snorm そのまま(-1..1)。",
}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _mv_snorm(mv):
    return ddsio.snorm16(np.clip(mv / MV_RANGE, -1, 1))


def _save(outdir, name, dxgi, arr, info, files):
    path = os.path.join(outdir, name)
    ddsio.write_dds(path, dxgi, arr)
    h, w = arr.shape[-3], arr.shape[-2]
    entry = dict(info, file=name, dxgi=dxgi, width=int(w), height=int(h), bytes=os.path.getsize(path), sha256=sha256_file(path))
    if arr.ndim == 4:
        entry["depth"] = int(arr.shape[0])
    files[name] = entry


def bake_fire(st, outdir, files):
    seq = fb.loop_blend(fb.bake_sequence("fire", st), st)
    atlas = fb.pack_atlas(seq["tex"], st.cols, st.rows)
    mv = fb.pack_atlas(seq["mv"], st.cols, st.rows)
    common = dict(cols=st.cols, rows=st.rows, frames=st.frames, fps=FPS)
    _save(outdir, "fire_flipbook.dds", ddsio.DXGI_R8G8B8A8_UNORM, ddsio.unorm8(atlas),
          dict(common, kind="fireFamily", frameSize=st.res), files)
    _save(outdir, "fire_mv.dds", ddsio.DXGI_R16G16_SNORM, _mv_snorm(mv),
          dict(common, kind="motionVectors", frameSize=st.mv_res, mvRange=MV_RANGE, pairsWith="fire_flipbook.dds"), files)


def bake_smoke(st, outdir, files):
    seq = fb.loop_blend(fb.bake_sequence("smoke", st), st)
    common = dict(cols=st.cols, rows=st.rows, frames=st.frames, fps=FPS)
    _save(outdir, "smoke_6way_rlu.dds", ddsio.DXGI_R8G8B8A8_UNORM, ddsio.unorm8(fb.pack_atlas(seq["tex"], st.cols, st.rows)),
          dict(common, kind="sixWayRLU", frameSize=st.res, pairsWith="smoke_6way_dbf.dds"), files)
    _save(outdir, "smoke_6way_dbf.dds", ddsio.DXGI_R8G8B8A8_UNORM, ddsio.unorm8(fb.pack_atlas(seq["tex2"], st.cols, st.rows)),
          dict(common, kind="sixWayDBF", frameSize=st.res, pairsWith="smoke_6way_rlu.dds"), files)
    _save(outdir, "smoke_mv.dds", ddsio.DXGI_R16G16_SNORM, _mv_snorm(fb.pack_atlas(seq["mv"], st.cols, st.rows)),
          dict(common, kind="motionVectors", frameSize=st.mv_res, mvRange=MV_RANGE, pairsWith="smoke_6way_rlu.dds"), files)


def bake_sparks(quick, outdir, files):
    res = 32 if quick else 64
    fr = sprites.sparks(8, res)
    _save(outdir, "sparks.dds", ddsio.DXGI_R8G8B8A8_UNORM, ddsio.unorm8(fb.pack_atlas(fr, 4, 2)),
          dict(kind="fireFamily", cols=4, rows=2, frames=8, frameSize=res,
               note="枠は寿命順。筋の頭が +u(右)。ストレッチ方向は速度に合わせて回す。"), files)


def bake_embers(quick, outdir, files):
    res = 32 if quick else 64
    fr = sprites.embers(16, res)
    _save(outdir, "embers.dds", ddsio.DXGI_R8G8B8A8_UNORM, ddsio.unorm8(fb.pack_atlas(fr, 4, 4)),
          dict(kind="fireFamily", cols=4, rows=4, frames=16, frameSize=res, note="枠は寿命順(熱が引き、灰の殻が広がる)。"), files)


def bake_curl(quick, outdir, files):
    n = 32 if quick else 64
    vol = sprites.curl_field(n)
    _save(outdir, "curl64.dds" if not quick else "curl32.dds", ddsio.DXGI_R16G16B16A16_SNORM, ddsio.snorm16(vol),
          dict(kind="curl3D", size=n, wrap="periodic"), files)


def run(outdir, quick, only, preview=None):
    st = fb.QUICK if quick else fb.FULL
    os.makedirs(outdir, exist_ok=True)
    files, times = {}, {}
    mpath = os.path.join(outdir, "vfx_bake_manifest.json")
    if os.path.exists(mpath) and set(only) != set(ALL):  # 一部だけ焼き直すときは、残りの項目を残す
        with open(mpath, encoding="utf-8") as f:
            prev = json.load(f)
        if prev.get("quick") == quick and prev.get("generatorVersion") == GENERATOR_VERSION:
            files.update({k: v for k, v in prev["files"].items() if os.path.exists(os.path.join(outdir, k))})
    for name in only:
        t0 = time.time()
        if name == "fire":
            bake_fire(st, outdir, files)
        elif name == "smoke":
            bake_smoke(st, outdir, files)
        elif name == "sparks":
            bake_sparks(quick, outdir, files)
        elif name == "embers":
            bake_embers(quick, outdir, files)
        elif name == "curl":
            bake_curl(quick, outdir, files)
        times[name] = round(time.time() - t0, 1)
        print(f"  {name}: {times[name]}s", flush=True)
    manifest = {
        "generatorVersion": GENERATOR_VERSION,
        "quick": quick,
        "license": "CC0-1.0(自作・手続き生成)",
        "conventions": CONVENTIONS,
        "settings": {"grid": st.grid, "res": st.res, "mvRes": st.mv_res, "cols": st.cols, "rows": st.rows, "spf": st.spf,
                     "overlap": st.overlap, "warmup": st.warmup, "seed": st.seed, "mvGain": fb.MV_GAIN},
        "files": dict(sorted(files.items())),
    }
    with open(os.path.join(outdir, "vfx_bake_manifest.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")
    if preview:
        write_previews(outdir, preview)
    return manifest


def write_previews(outdir, pdir):
    """目視用の PNG(コミットしない)。色は RGB、A は別ファイル。MV と SNORM は 0.5 中心に写す。"""
    from PIL import Image
    os.makedirs(pdir, exist_ok=True)
    for fn in sorted(os.listdir(outdir)):
        if not fn.endswith(".dds"):
            continue
        dxgi, a = ddsio.read_dds(os.path.join(outdir, fn))
        if a.ndim == 4:
            a = a[a.shape[0] // 2]
        if dxgi == ddsio.DXGI_R8G8B8A8_UNORM:
            f = a.astype(np.float32) / 255.0
        else:
            f = a.astype(np.float32) / 32767.0 * 0.5 + 0.5
        rgb = np.concatenate([f, np.full(f.shape[:2] + (3 - f.shape[2],), 0.5, np.float32)], axis=-1)[..., :3] if f.shape[2] < 3 else f[..., :3]
        Image.fromarray((np.clip(rgb, 0, 1) * 255).astype("u1")).save(os.path.join(pdir, fn[:-4] + ".png"))
        if dxgi == ddsio.DXGI_R8G8B8A8_UNORM:
            Image.fromarray((np.clip(f[..., 3], 0, 1) * 255).astype("u1")).save(os.path.join(pdir, fn[:-4] + "_a.png"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--only")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--preview")
    a = ap.parse_args()
    only = tuple(a.only.split(",")) if a.only else ALL
    for o in only:
        if o not in ALL:
            ap.error(f"未知の項目: {o}(使えるもの: {','.join(ALL)})")
    if a.verify:
        with open(os.path.join(DEFAULT_OUT, "vfx_bake_manifest.json"), encoding="utf-8") as f:
            committed = json.load(f)
        with tempfile.TemporaryDirectory() as tmp:
            m = run(tmp, False, only)
        bad = [k for k, v in m["files"].items() if committed["files"].get(k, {}).get("sha256") != v["sha256"]]
        print("一致" if not bad else "不一致: " + ", ".join(bad))
        return 1 if bad else 0
    out = a.out or (os.path.join(tempfile.gettempdir(), "vfx_bake_quick") if a.quick else DEFAULT_OUT)
    t0 = time.time()
    print(f"焼き込み({'quick' if a.quick else 'full'}) → {out}")
    run(out, a.quick, only, a.preview)
    print(f"完了 {time.time() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
