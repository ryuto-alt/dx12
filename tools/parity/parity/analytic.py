"""Q2: 閉形式(解析解)との照合。パストレーサーを介さず「物理の式そのもの」でエンジンの線形出力を検算する。

  * ランバート平面 + 点光源(逆二乗): 平面上の各画素について
        L = albedo/π · Σ_i I_i · max(cosθ_i, 0) / d_i²        [nit]   (I_i = 光度 cd)
    を計算し、エンジンの線形画像(物理ライティング単位 = nit)と比べる。
  * カメラはエンジンの set_editor_camera と同じ: 左手系 Y-up・LookAtLH・垂直 FOV(既定 45°)・アスペクト = 幅/高さ。

これは**拡散だけの理想**なので、エンジン画像との差は「鏡面(F0=0.04 の GGX がわずかに足される)・環境光・相互反射」の分だけ出る。
仕様の `analytic` に許容(median / p95 の相対誤差)を書いて、逆二乗と cosθ と光度の単位が式どおりに効いているかを機械判定する。
UE も PT も要らない(UE は再インストールしない方針)。
"""
from __future__ import annotations

import numpy as np


def camera_rays(position, target, width: int, height: int, fov_deg: float = 45.0, up=(0.0, 1.0, 0.0)) -> tuple[np.ndarray, np.ndarray]:
    """画素中心を通る視線(原点 = カメラ位置、方向 = 正規化)。戻り値 (origin[3], dirs[H,W,3])。左手系 LookAtLH。"""
    eye = np.asarray(position, dtype=np.float64)
    fwd = np.asarray(target, dtype=np.float64) - eye
    fwd /= np.linalg.norm(fwd)
    right = np.cross(np.asarray(up, dtype=np.float64), fwd)
    right /= np.linalg.norm(right)
    upv = np.cross(fwd, right)
    t = np.tan(np.radians(fov_deg) / 2.0)
    aspect = width / height
    xs = (2.0 * (np.arange(width) + 0.5) / width - 1.0) * t * aspect
    ys = (1.0 - 2.0 * (np.arange(height) + 0.5) / height) * t
    d = xs[None, :, None] * right[None, None, :] + ys[:, None, None] * upv[None, None, :] + fwd[None, None, :]
    d /= np.linalg.norm(d, axis=-1, keepdims=True)
    return eye, d


def lambert_plane_point_lights(cam: dict, size: tuple[int, int], plane: dict, lights: list[dict]) -> tuple[np.ndarray, np.ndarray]:
    """平面の期待放射輝度 [nit]。戻り値 (L[H,W] (平面に当たらない画素は 0), mask[H,W] (当たった画素))。

    cam    = {position, target, fovDeg?}
    plane  = {point, normal, albedo, bounds?: {axis: [min, max]...}(平面上の有効範囲を世界座標の軸で切る。例 {"x":[-5,5],"y":[0.05,3.5]})}
    lights = [{pos, cd}]
    """
    w, h = size
    eye, dirs = camera_rays(cam["position"], cam["target"], w, h, float(cam.get("fovDeg", 45.0)))
    n = np.asarray(plane["normal"], dtype=np.float64)
    n /= np.linalg.norm(n)
    p0 = np.asarray(plane["point"], dtype=np.float64)
    denom = dirs @ n
    with np.errstate(divide="ignore", invalid="ignore"):
        tt = ((p0 - eye) @ n) / denom
    hit = eye[None, None, :] + dirs * tt[..., None]
    mask = (denom != 0) & (tt > 0)
    for ax, (lo, hi) in (plane.get("bounds") or {}).items():
        i = "xyz".index(ax)
        mask &= (hit[..., i] >= lo) & (hit[..., i] <= hi)
    L = np.zeros((h, w), dtype=np.float64)
    for li in lights:
        lp = np.asarray(li["pos"], dtype=np.float64)
        to = lp[None, None, :] - hit
        d2 = np.maximum((to * to).sum(-1), 1e-12)
        d = np.sqrt(d2)
        cos = np.maximum((to @ n) / d, 0.0)
        L += float(li["cd"]) * cos / d2
    L *= float(plane["albedo"]) / np.pi
    L[~mask] = 0.0
    return L, mask


def compare(expected: np.ndarray, mask: np.ndarray, lin_rgb: np.ndarray, min_luminance: float = 0.0) -> dict:
    """エンジンの線形画像(グレー面なので輝度 = 平均チャンネル)と期待値の相対誤差の統計。min_luminance 未満の画素は除く(暗部の量子化・環境光)。"""
    lum = lin_rgb.mean(-1).astype(np.float64)
    sel = mask & (expected > max(min_luminance, 1e-9))
    if not sel.any():
        return {"pixels": 0, "median_rel": None, "p95_rel": None, "mean_rel": None, "bias": None}
    rel = (lum[sel] - expected[sel]) / expected[sel]
    return {"pixels": int(sel.sum()), "median_rel": float(np.median(np.abs(rel))), "p95_rel": float(np.percentile(np.abs(rel), 95)),
            "mean_rel": float(np.mean(np.abs(rel))), "bias": float(np.median(rel)),
            "expected_range_nit": [float(expected[sel].min()), float(expected[sel].max())]}


def check(spec_analytic: dict, cameras: dict[str, dict], camera_name: str, lin_rgb: np.ndarray) -> dict | None:
    """仕様の analytic[] のうち camera_name に該当するものを評価する。無ければ None。"""
    out = []
    for a in spec_analytic if isinstance(spec_analytic, list) else [spec_analytic]:
        if a.get("camera") != camera_name:
            continue
        cam = cameras[camera_name]
        h, w = lin_rgb.shape[:2]
        if a["kind"] != "lambert_plane_point_lights":
            raise ValueError(f"analytic.kind が不明: {a['kind']}")
        expected, mask = lambert_plane_point_lights(cam, (w, h), a["plane"], a["lights"])
        stats = compare(expected, mask, lin_rgb, float(a.get("minLuminanceNit", 0.0)))
        tol = a.get("tolerance") or {}
        reasons = []
        if stats["pixels"] == 0:
            reasons.append("平面に当たる画素が無い(カメラと平面の指定を確認)")
        for k in ("median_rel", "p95_rel", "mean_rel"):
            if k in tol and stats.get(k) is not None and stats[k] > float(tol[k]):
                reasons.append(f"analytic: {k} = {stats[k]:.3g} が許容 {float(tol[k]):g} を超えた")
        if "bias_abs" in tol and stats.get("bias") is not None and abs(stats["bias"]) > float(tol["bias_abs"]):
            reasons.append(f"analytic: 系統誤差 bias = {stats['bias']:+.3g} が許容 ±{float(tol['bias_abs']):g} を超えた")
        out.append({"kind": a["kind"], "name": a.get("name", a["kind"]), **stats, "tolerance": tol, "reasons": reasons})
    return {"checks": out, "reasons": [r for c in out for r in c["reasons"]]} if out else None
