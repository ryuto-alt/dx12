"""1 組(基準 vs エンジン画像)の評価: 整列 -> 指標 -> 領域別 -> ヒートマップ -> 判定。compare / run / baseline の共通部品。"""
from __future__ import annotations

import hashlib
from pathlib import Path

import numpy as np

from . import gate as G
from . import heatmap as H
from . import metrics as M
from . import regions as R
from .imgio import Img, ensure_same_size, save_png, to_u8
from .tonemap import align_pair, apply_ev, apply_tonemap, parse_exposure
from . import color as _color


def sha256_u8(display: np.ndarray) -> str:
    return hashlib.sha256(to_u8(display).tobytes()).hexdigest()


def diff_bbox(a_display: np.ndarray, b_display: np.ndarray, tol_lsb: int = 2) -> dict | None:
    """8bit で tol_lsb を超えて違う画素の外接矩形。無ければ None。"""
    d = np.abs(to_u8(a_display).astype(np.int16) - to_u8(b_display).astype(np.int16)).max(axis=2) > tol_lsb
    if not d.any():
        return None
    ys, xs = np.where(d)
    return {"x": int(xs.min()), "y": int(ys.min()), "w": int(xs.max() - xs.min() + 1), "h": int(ys.max() - ys.min() + 1),
            "pixels": int(d.sum()), "ratio": float(d.mean())}


def display_check(lin: Img, disp: Img, tonemap: str, exposure) -> dict:
    """Q2: エンジンの線形 float(PFM)にハーネスの numpy 実装で露出 + トーンマップを掛けた期待値と、エンジン自身の表示 PNG
    (GPU のトーンマップ + 露出)を 8bit で比べる。両者が同じ式なら差は量子化の丸め(1 LSB 以内)だけ。
    exposure は alignment.exposure と同じ書式(ev100:15 など。エンジン側の適用 EV は test 側の値を使う)。"""
    ex = parse_exposure(exposure)
    expected, eotf = apply_tonemap(tonemap, apply_ev(lin.rgb, ex["ev_test"]))
    disp_rgb = disp.rgb
    if disp.eotf != eotf:                        # PNG のエンコードが違うなら、リニアを経由して合わせる(差が出るのは仕様どおり)
        disp_rgb = _color.encode(disp.to_linear(), eotf)
    h = min(expected.shape[0], disp_rgb.shape[0])
    w = min(expected.shape[1], disp_rgb.shape[1])
    d = np.abs(to_u8(expected[:h, :w]).astype(np.int16) - to_u8(disp_rgb[:h, :w]).astype(np.int16)).max(axis=2).astype(np.float32)
    return {
        "mean_lsb": float(d.mean()), "p99_lsb": float(np.percentile(d, 99.0)), "max_lsb": float(d.max()),
        "frac_gt2": float((d > 2).mean()), "eotf_expected": eotf, "eotf_png": disp.eotf, "tonemap": tonemap,
        "ev_applied": ex["ev_test"],
    }


def evaluate_pair(ref: Img, test: Img, out_dir: Path, *, tonemap: str = "engine_aces", exposure=None,
                  size_policy: str = "error", metrics=None, ppd: float | None = None, flip_hdr_params: dict | None = None,
                  regions: list[dict] | None = None, region_base: Path | None = None,
                  gate_name: str | None = None, gate_full: dict | None = None, gate_regions: dict | None = None,
                  floors: dict | None = None, k: float = 1.5, strict: bool = False, regression: bool = False,
                  ref_label: str = "基準", test_label: str = "エンジン", rel_base: Path | None = None,
                  heat_vmax: float = 1.0, write_images: bool = True) -> dict:
    """戻り値はそのまま run.json のカメラ 1 件になる(画像パスは rel_base からの相対)。"""
    out_dir = Path(out_dir)
    rel_base = rel_base or out_dir
    mets = M.normalize_metrics(metrics)
    ref, test, size_info = ensure_same_size(ref, test, size_policy)

    reg_masks = R.load_regions(regions or [], test.size, region_base) if regions else {}
    al = align_pair(ref, test, tonemap, exposure)
    if metrics is None and al.ref_linear is not None and "flip-hdr" not in mets:
        mets.append("flip-hdr")             # 両方がリニア HDR なら HDR-FLIP も自動で計算する
    maps = M.compute_maps(al, mets, ppd=ppd, flip_hdr_params=flip_hdr_params)
    ev_applied = al.info.get("exposure_ev_applied")
    full = M.summarize(maps, None, mets, exposure_ev_applied=ev_applied)
    per_region = {n: M.summarize(maps, m, mets) for n, m in reg_masks.items()}
    per_region = {n: v for n, v in per_region.items() if v}

    res: dict = {"metrics": full, "regions": per_region, "align": al.info, "size": size_info, "notes": list(maps.notes),
                 "flip": {"ldr": maps.flip_ldr_params, "hdr": maps.flip_hdr_params},
                 "sanity": G.sanity(maps.test_canon, maps.ref_canon)}
    if "flip_ldr" in maps.maps:
        res["worstTiles"] = H.worst_tiles(maps.maps["flip_ldr"])
    if regression:
        res["bitExact"] = bool(np.array_equal(to_u8(maps.ref_canon), to_u8(maps.test_canon)))
        res["diffBBox"] = diff_bbox(maps.ref_canon, maps.test_canon)

    if write_images:
        out_dir.mkdir(parents=True, exist_ok=True)
        rel = lambda p: str(Path(p).resolve().relative_to(Path(rel_base).resolve())).replace("\\", "/")  # noqa: E731
        imgs: dict = {}
        imgs["ref"] = rel(save_png(out_dir / "ref.png", maps.ref_canon))
        imgs["test"] = rel(save_png(out_dir / "test.png", maps.test_canon))
        panels = [(ref_label, maps.ref_canon), (test_label, maps.test_canon)]
        if "flip_ldr" in maps.maps:
            hm = H.save_error_heatmap(out_dir / "heat_flip_ldr.png", maps.maps["flip_ldr"], heat_vmax, "LDR-FLIP")
            imgs["heat_flip_ldr"] = rel(hm)
            panels.append(("LDR-FLIP", H.add_colorbar(H.magma(maps.maps["flip_ldr"] / heat_vmax), heat_vmax, "FLIP")))
        if "flip_hdr" in maps.maps:
            imgs["heat_flip_hdr"] = rel(H.save_error_heatmap(out_dir / "heat_flip_hdr.png", maps.maps["flip_hdr"], heat_vmax, "HDR-FLIP"))
        if "de2000" in maps.maps:
            imgs["heat_de2000"] = rel(H.save_error_heatmap(out_dir / "heat_de2000.png", maps.maps["de2000"], 10.0, "dE2000"))
        imgs["diff"] = rel(save_png(out_dir / "diff.png", H.diff_image(maps.ref_canon, maps.test_canon)))
        imgs["diff_signed"] = rel(save_png(out_dir / "diff_signed.png", H.signed_luma_diff(maps.ref_canon, maps.test_canon)))
        imgs["contact"] = rel(H.contact_sheet(panels, out_dir / "contact.png"))
        res["images"] = imgs

    if gate_name is not None:
        res["verdict"] = G.judge_camera(full, per_region, gate_name, gate_full or {}, gate_regions or {}, floors, k,
                                        res["sanity"], strict)
        if regression and not res["bitExact"]:
            res["verdict"]["warnings"].append("ビット単位では一致していない(diffBBox を参照)")
    return res
