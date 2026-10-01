"""GI 評価ハーネス(S1)の指標と合否判定(A1〜A4)。純粋な numpy。エンジンにも画像ファイルにも依存しない(単体テストは合成画像で)。

入力はどれも**線形 float RGB(Rec.709・露出 1.0・トーンマップ前)**: ラスタの `screenshot_final {format:"pfm"}` と PT の `render_reference` の PFM。

指標の定義(領域マスクごと。マスクは gi_scene.build_masks):
  y_*            輝度 Y = 0.2126 R + 0.7152 G + 0.0722 B の領域平均(raster / pt)
  ev_mean        log2( (mean Yr + ε) / (mean Ypt + ε) )   ← A1 / A2 の判定に使う「平均輝度の EV 差」。+ = ラスタが明るい。ε = 2^-8
  ev_logmean     平均( log2((Yr+ε)/(Ypt+ε)) )               ← 画素ごとの対数差の平均(暗部の違いも拾う。参考)
  black_rate_*   Y < 2^-8 の画素の割合(「黒潰れ率」。ε と同じ値 = 8bit の約 1/256 より暗い)
  rg_*           領域平均 R / 領域平均 G(色の照り返しの向き)
  flip_hdr_mean  HDR-FLIP(PT = 基準・ラスタ = テスト)の領域平均(tools/parity の既存実装)
"""
from __future__ import annotations

import math

import numpy as np

EV_EPS = 2.0 ** -8
BLACK_Y = 2.0 ** -8
MIN_PIXELS = 400           # これ未満の領域は「判定不能」(誤差が大きい)

# 合否のしきい値(設計書 docs/GI_FOUNDATION_DESIGN.md §1)
A1_EV_ABS = 0.5
A2_BLACK_PT = 10.0         # 黒潰れ率の差(ポイント)
A2_EV_MAX = 0.5
A3_RATIO_TOL = 0.20
A4_EV_MAX = -1.0


def luminance(rgb: np.ndarray) -> np.ndarray:
    return (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]).astype(np.float64)


def ev_ratio(a: float, b: float, eps: float = EV_EPS) -> float:
    return math.log2((a + eps) / (b + eps))


def region_stats(raster: np.ndarray, pt: np.ndarray, mask: np.ndarray, flip_map: np.ndarray | None = None) -> dict:
    n = int(mask.sum())
    if n == 0:
        return {"pixels": 0, "evaluable": False}
    yr, yp = luminance(raster)[mask], luminance(pt)[mask]
    r, p = raster[mask].astype(np.float64), pt[mask].astype(np.float64)
    mr, mp = float(yr.mean()), float(yp.mean())
    rg_r = float(r[:, 0].mean() / max(r[:, 1].mean(), 1e-12))
    rg_p = float(p[:, 0].mean() / max(p[:, 1].mean(), 1e-12))
    out = {
        "pixels": n,
        "evaluable": n >= MIN_PIXELS,
        "y_raster": mr, "y_pt": mp,
        "ev_mean": ev_ratio(mr, mp),
        "ev_logmean": float(np.mean(np.log2((yr + EV_EPS) / (yp + EV_EPS)))),
        "black_rate_raster": float((yr < BLACK_Y).mean()),
        "black_rate_pt": float((yp < BLACK_Y).mean()),
        "rgb_raster": [float(x) for x in r.mean(axis=0)],
        "rgb_pt": [float(x) for x in p.mean(axis=0)],
        "rg_raster": rg_r, "rg_pt": rg_p,
    }
    if flip_map is not None:
        out["flip_hdr_mean"] = float(flip_map[mask].mean())
    return out


def all_region_stats(raster: np.ndarray, pt: np.ndarray, masks: dict[str, np.ndarray], flip_map: np.ndarray | None = None) -> dict:
    return {name: region_stats(raster, pt, m, flip_map) for name, m in masks.items()}


def global_stats(raster: np.ndarray, pt: np.ndarray, flip_map: np.ndarray | None = None) -> dict:
    full = np.ones(raster.shape[:2], bool)
    s = region_stats(raster, pt, full, flip_map)
    s["nan_raster"] = int((~np.isfinite(raster)).sum())
    s["nan_pt"] = int((~np.isfinite(pt)).sum())
    return s


# ── 合否 A1〜A4 ──────────────────────────────────────────────────────────────

PASS, FAIL, NA, NOT_IMPL, UNJUDGEABLE = "PASS", "FAIL", "N/A", "未実装", "判定不能"


def judge_a1(stats_by_cam: dict[str, dict], regions: list[str]) -> dict:
    """A1: 指定領域(壁・天井・床)の平均輝度が PT と ±0.5EV 以内。stats_by_cam = {カメラ: {領域: region_stats}}。"""
    rows, bad, missing = {}, [], []
    for cam, st in stats_by_cam.items():
        for r in regions:
            s = st.get(r)
            if not s or not s.get("evaluable"):
                missing.append(f"{cam}/{r}")
                continue
            ok = abs(s["ev_mean"]) <= A1_EV_ABS
            rows[f"{cam}/{r}"] = {"ev_mean": s["ev_mean"], "ok": ok}
            if not ok:
                bad.append(f"{cam}/{r} {s['ev_mean']:+.2f}EV")
    if not rows:
        return {"status": UNJUDGEABLE, "detail": rows, "reason": "評価できる領域が無い: " + ", ".join(missing)}
    return {"status": FAIL if bad else PASS, "detail": rows,
            "reason": ("範囲外: " + ", ".join(bad)) if bad else f"全領域 |EV| ≤ {A1_EV_ABS}",
            "worst_ev": max((abs(v["ev_mean"]) for v in rows.values()), default=0.0), "missing": missing}


def judge_a2(s: dict | None) -> dict:
    """A2: 暗い側の黒潰れ率が PT 比 ±10pt 以内、かつ平均輝度が PT より +0.5EV を超えて明るくない。"""
    if not s or not s.get("evaluable"):
        return {"status": UNJUDGEABLE, "reason": "領域が小さすぎる / 無い"}
    d_black = (s["black_rate_raster"] - s["black_rate_pt"]) * 100.0
    ok_black = abs(d_black) <= A2_BLACK_PT
    ok_ev = s["ev_mean"] <= A2_EV_MAX
    return {"status": PASS if (ok_black and ok_ev) else FAIL,
            "black_rate_diff_pt": d_black, "ev_mean": s["ev_mean"],
            "black_rate_raster": s["black_rate_raster"], "black_rate_pt": s["black_rate_pt"],
            "reason": f"黒潰れ率差 {d_black:+.1f}pt(許容 ±{A2_BLACK_PT:g})・平均輝度 {s['ev_mean']:+.2f}EV(許容 ≤ +{A2_EV_MAX:g})"}


def judge_a3(s: dict | None) -> dict:
    """A3: 天井の R/G 比が PT と同じ向き(PT が R>G ならラスタも R>G)で、比の差が 20% 以内。"""
    if not s or not s.get("evaluable"):
        return {"status": UNJUDGEABLE, "reason": "天井の領域が小さすぎる / 無い"}
    rp, rr = s["rg_pt"], s["rg_raster"]
    if rp <= 1.0:
        return {"status": UNJUDGEABLE, "rg_pt": rp, "rg_raster": rr, "reason": f"PT 側が R>G でない(R/G = {rp:.3f})。シーンの前提が崩れている"}
    same_dir = rr > 1.0
    rel = abs(rr / rp - 1.0)
    ok = same_dir and rel <= A3_RATIO_TOL
    return {"status": PASS if ok else FAIL, "rg_pt": rp, "rg_raster": rr, "rel_diff": rel,
            "reason": f"R/G ラスタ {rr:.3f} / PT {rp:.3f}(差 {rel * 100:.0f}%・許容 {A3_RATIO_TOL * 100:.0f}% かつ R>G)"}


def judge_a4(s_cfg: dict | None, s_legacy: dict | None, is_baseline: bool) -> dict:
    """A4: クロム球領域の平均輝度が既定(legacy)比 -1EV 以上暗い。"""
    if is_baseline:
        return {"status": NA, "reason": "既定(legacy)自身が基準"}
    if not s_cfg or not s_legacy or not s_cfg.get("evaluable") or not s_legacy.get("evaluable"):
        return {"status": UNJUDGEABLE, "reason": "クロム球の領域が小さすぎる / legacy が無い"}
    ev = ev_ratio(s_cfg["y_raster"], s_legacy["y_raster"])
    return {"status": PASS if ev <= A4_EV_MAX else FAIL, "ev_vs_legacy": ev,
            "y_cfg": s_cfg["y_raster"], "y_legacy": s_legacy["y_raster"], "y_pt": s_cfg["y_pt"],
            "reason": f"クロム球の平均輝度 既定比 {ev:+.2f}EV(許容 ≤ {A4_EV_MAX:g})・参考: PT は {ev_ratio(s_cfg['y_pt'], s_legacy['y_raster']):+.2f}EV"}


def judge_information(stats_by_cam: dict[str, dict], regions: list[str]) -> dict:
    """情報のみ(合否に数えない): 指定領域の ev_mean を ±0.5EV の目安で表にする。"""
    rows = {}
    for cam, st in stats_by_cam.items():
        for r in regions:
            s = st.get(r)
            if s and s.get("evaluable"):
                rows[f"{cam}/{r}"] = {"ev_mean": s["ev_mean"], "within_0p5": abs(s["ev_mean"]) <= 0.5}
    worst = max((abs(v["ev_mean"]) for v in rows.values()), default=None)
    return {"status": "情報", "detail": rows, "worst_ev": worst,
            "reason": "全領域 ±0.5EV 以内" if rows and worst is not None and worst <= 0.5 else "±0.5EV を超える領域あり"}
