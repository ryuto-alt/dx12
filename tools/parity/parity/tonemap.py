"""トーンマップと露出の揃え。

比較の前に「エンジン画像」と「基準」を同じ土俵(同じトーンマップ・同じ露出)に載せる。

* リニア HDR の画像(PFM/EXR)にだけトーンマップを掛ける。PNG(表示参照)はもう掛かっているので触らない。
* 露出は 3 モード: none / fixed_ev / auto_median(中央値合わせ)。
  auto_median は「露出の違い」を指標から消してしまうので、適用した EV は必ず結果に残し(alignment.ev_applied)、
  ゲートでは exposure_ev_abs_max で「補正しすぎ」を落とせるようにしてある。

トーンマップ候補(全て自前実装。UE のシェーダ由来のコードは 1 行も入れていない):
  linear_clip      クリップ + sRGB(トーンマップ無し)
  engine_aces      Uno の PostProcess.hlsl と同じ: Narkowicz ACES → pow(1/2.2)   (eotf=gamma22)
  aces_narkowicz   Narkowicz ACES → sRGB OETF
  aces_hill        Stephen Hill の ACES RRT+ODT フィット → sRGB OETF
  ue_filmic_approx UE 既定の Filmic は ACES 準拠に設計されているので aces_hill の別名(近似。校正で残差を見る)
  reinhard         x/(1+x) → sRGB
  engine_agx       Uno の AgX(Wrensch フィット。PostProcess.hlsl と同じ係数)  (eotf=gamma22)
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from . import color
from .imgio import Img


# ── 演算子 ──────────────────────────────────────────────────────────────────

def _aces_narkowicz_curve(x: np.ndarray) -> np.ndarray:
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    return np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0)


_HILL_IN = np.array([[0.59719, 0.35458, 0.04823],
                     [0.07600, 0.90834, 0.01566],
                     [0.02840, 0.13383, 0.83777]], dtype=np.float32)
_HILL_OUT = np.array([[1.60475, -0.53108, -0.07367],
                      [-0.10208, 1.10813, -0.00605],
                      [-0.00327, -0.07276, 1.07602]], dtype=np.float32)


def _aces_hill_curve(x: np.ndarray) -> np.ndarray:
    v = x @ _HILL_IN.T
    v = (v * (v + 0.0245786) - 0.000090537) / (v * (0.983729 * v + 0.4329510) + 0.238081)
    return np.clip(v @ _HILL_OUT.T, 0.0, 1.0)


_AGX_MAT = np.array([[0.842479062253094, 0.0784335999999992, 0.0792237451477643],
                     [0.0423282422610123, 0.878468636469772, 0.0791661274605434],
                     [0.0423756549057051, 0.0784336, 0.879142973793104]], dtype=np.float32)
_AGX_INV = np.array([[1.19687900512017, -0.0980208811401368, -0.0990297440797205],
                     [-0.0528968517574562, 1.15190312990417, -0.0989611768448433],
                     [-0.0529716355144438, -0.0980434501171241, 1.15107367264116]], dtype=np.float32)


def _agx(x: np.ndarray) -> np.ndarray:
    min_ev, max_ev = -12.47393, 4.026069
    v = np.maximum(x, 0.0) @ _AGX_MAT.T
    v = np.clip(np.log2(np.maximum(v, 1e-10)), min_ev, max_ev)
    v = (v - min_ev) / (max_ev - min_ev)
    v2 = v * v
    v4 = v2 * v2
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232
    return np.clip(v @ _AGX_INV.T, 0.0, 1.0)


@dataclass(frozen=True)
class Tonemap:
    name: str
    fn: callable          # リニア(露出済み)-> ガンマ前のトーン後リニア(0..1)
    eotf: str             # 出力のエンコード
    note: str


def _mk() -> dict[str, Tonemap]:
    t: dict[str, Tonemap] = {}
    t["linear_clip"] = Tonemap("linear_clip", lambda x: np.clip(x, 0.0, 1.0), "srgb", "トーンマップ無し(1 でクリップ)")
    t["engine_aces"] = Tonemap("engine_aces", _aces_narkowicz_curve, "gamma22", "Uno の既定(Narkowicz ACES + pow(1/2.2))")
    t["aces_narkowicz"] = Tonemap("aces_narkowicz", _aces_narkowicz_curve, "srgb", "Narkowicz ACES + sRGB")
    t["aces_hill"] = Tonemap("aces_hill", _aces_hill_curve, "srgb", "Hill の ACES RRT+ODT フィット + sRGB")
    t["ue_filmic_approx"] = Tonemap("ue_filmic_approx", _aces_hill_curve, "srgb", "UE 既定 Filmic の近似(ACES 準拠設計なので aces_hill を使う)")
    t["reinhard"] = Tonemap("reinhard", lambda x: np.clip(x / (1.0 + x), 0.0, 1.0), "srgb", "Reinhard + sRGB")
    t["engine_agx"] = Tonemap("engine_agx", _agx, "gamma22", "Uno の AgX(PostProcess.hlsl と同じ係数)")
    return t


TONEMAPS: dict[str, Tonemap] = _mk()


def list_tonemaps() -> list[str]:
    return list(TONEMAPS)


def get_tonemap(name: str) -> Tonemap:
    key = (name or "engine_aces").lower()
    if key == "none":
        key = "linear_clip"
    if key not in TONEMAPS:
        raise ValueError(f"トーンマップが不明: {name}(候補: {', '.join(TONEMAPS)})")
    return TONEMAPS[key]


def apply_tonemap(name: str, linear: np.ndarray) -> tuple[np.ndarray, str]:
    """リニア(露出済み)-> (表示参照 0..1 のエンコード済み画素, その eotf)。"""
    tm = get_tonemap(name)
    y = tm.fn(np.maximum(np.asarray(linear, dtype=np.float32), 0.0))
    return color.encode(y, tm.eotf).astype(np.float32), tm.eotf


def apply_ev(linear: np.ndarray, ev: float) -> np.ndarray:
    return (np.asarray(linear, dtype=np.float32) * np.float32(2.0 ** ev)).astype(np.float32)


# ── 露出の当て方 ────────────────────────────────────────────────────────────

def parse_exposure(spec) -> dict:
    """'none' | 'auto' | 'auto_median' | 'fixed:+1.5' | {'mode':..., 'ev':..., 'ev_ref':..., 'ev_test':...} -> 正規化した dict。"""
    if spec is None:
        return {"mode": "none", "ev_ref": 0.0, "ev_test": 0.0}
    if isinstance(spec, (int, float)):
        return {"mode": "fixed_ev", "ev_ref": 0.0, "ev_test": float(spec)}
    if isinstance(spec, str):
        s = spec.strip().lower()
        if s in ("none", ""):
            return {"mode": "none", "ev_ref": 0.0, "ev_test": 0.0}
        if s in ("auto", "auto_median"):
            return {"mode": "auto_median", "ev_ref": 0.0, "ev_test": 0.0}
        if s.startswith("fixed:"):
            return {"mode": "fixed_ev", "ev_ref": 0.0, "ev_test": float(s.split(":", 1)[1])}
        raise ValueError(f"露出の指定が不明: {spec}(none / auto / fixed:EV)")
    d = dict(spec)
    mode = d.get("mode", "none")
    if mode == "fixed_ev" and "ev" in d and "ev_test" not in d:
        d["ev_test"] = d["ev"]
    return {"mode": mode, "ev_ref": float(d.get("ev_ref", 0.0)), "ev_test": float(d.get("ev_test", 0.0))}


def _sample(a: np.ndarray, mask: np.ndarray | None, max_px: int = 250_000) -> np.ndarray:
    flat = a.reshape(-1, *a.shape[2:]) if a.ndim > 2 else a.reshape(-1)
    if mask is not None:
        flat = flat[mask.reshape(-1)]
    if flat.shape[0] > max_px:
        flat = flat[:: flat.shape[0] // max_px + 1]
    return flat


def median_luminance_linear(lin: np.ndarray, mask: np.ndarray | None = None) -> float:
    y = _sample(color.luminance(lin), mask)
    y = y[y > 1e-6]
    return float(np.median(y)) if y.size else 0.0


def fit_ev_linear(src_lin: np.ndarray, ref_lin: np.ndarray, mask: np.ndarray | None = None) -> float:
    """src に掛けると中央輝度が ref と一致する EV。"""
    a, b = median_luminance_linear(src_lin, mask), median_luminance_linear(ref_lin, mask)
    if a <= 0 or b <= 0:
        return 0.0
    return float(np.log2(b / a))


def _median_display_luma(lin: np.ndarray, tonemap: str, ev: float, mask: np.ndarray | None) -> float:
    d, _ = apply_tonemap(tonemap, apply_ev(_sample(lin, mask), ev))
    return float(np.median(color.luma_display(d)))


def fit_ev_to_display(src_lin: np.ndarray, tonemap: str, target_luma_median: float, mask: np.ndarray | None = None,
                      lo: float = -10.0, hi: float = 10.0) -> float:
    """src(リニア)にトーンマップを掛けた表示の中央輝度が target になる EV(二分法。トーンマップは単調)。"""
    f = lambda ev: _median_display_luma(src_lin, tonemap, ev, mask)  # noqa: E731
    if f(lo) >= target_luma_median:
        return lo
    if f(hi) <= target_luma_median:
        return hi
    for _ in range(40):
        mid = 0.5 * (lo + hi)
        if f(mid) < target_luma_median:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


# ── ペアの整列 ──────────────────────────────────────────────────────────────

@dataclass
class Aligned:
    ref_display: np.ndarray
    test_display: np.ndarray
    ref_eotf: str
    test_eotf: str
    ref_linear: np.ndarray | None      # 両方リニアのときだけ(HDR-FLIP 用。露出適用済み)
    test_linear: np.ndarray | None
    info: dict


def align_pair(ref: Img, test: Img, tonemap: str = "engine_aces", exposure=None, mask: np.ndarray | None = None) -> Aligned:
    ex = parse_exposure(exposure)
    tm = get_tonemap(tonemap)
    info: dict = {"tonemap": tm.name, "exposure": ex["mode"], "refKind": ref.kind, "testKind": test.kind,
                  "ev_ref_applied": 0.0, "ev_test_applied": 0.0}
    ref_lin = ref.rgb if ref.kind == "linear" else None
    test_lin = test.rgb if test.kind == "linear" else None
    ev_ref, ev_test = ex["ev_ref"], ex["ev_test"]

    # 露出が掛かる側 = リニアの側。両方リニア / 両方 display なら test
    if ex["mode"] == "auto_median":
        if ref_lin is not None and test_lin is not None:
            ev_test = fit_ev_linear(test_lin, ref_lin, mask)
        elif ref_lin is not None:                                  # ref がリニア、test が display
            target = float(np.median(_sample(color.luma_display(test.rgb), mask)))
            ev_ref = fit_ev_to_display(ref_lin, tm.name, target, mask)
        elif test_lin is not None:                                 # test がリニア、ref が display
            target = float(np.median(_sample(color.luma_display(ref.rgb), mask)))
            ev_test = fit_ev_to_display(test_lin, tm.name, target, mask)
        else:                                                      # 両方 display: リニアに戻して中央輝度合わせ
            ev_test = fit_ev_linear(test.to_linear(), ref.to_linear(), mask)

    def to_display(im: Img, lin: np.ndarray | None, ev: float):
        if lin is not None:
            lin2 = apply_ev(lin, ev) if ev else lin
            d, e = apply_tonemap(tm.name, lin2)
            return d, e, lin2
        if ev:                                                     # display にリニア経由で露出を掛ける
            d = color.encode(np.clip(apply_ev(im.to_linear(), ev), 0.0, 1.0), im.eotf)
            return d, im.eotf, None
        return im.rgb, im.eotf, None

    rd, re_, rl = to_display(ref, ref_lin, ev_ref)
    td, te_, tl = to_display(test, test_lin, ev_test)
    info["ev_ref_applied"], info["ev_test_applied"] = float(ev_ref), float(ev_test)
    info["exposure_ev_applied"] = float(ev_test - ev_ref)          # test を ref へ寄せた実質の EV(符号付き)
    return Aligned(rd, td, re_, te_, rl if (rl is not None and tl is not None) else None,
                   tl if (rl is not None and tl is not None) else None, info)


# ── 校正(UE スクショ取込用) ────────────────────────────────────────────────

def luma_quantile_emd(a_display: np.ndarray, b_display: np.ndarray, n: int = 101) -> float:
    """表示輝度(ガンマ空間)分布の 1 次元 EMD(= 分位関数の L1 差)。0..1。"""
    qa = np.quantile(_sample(color.luma_display(a_display), None), np.linspace(0, 1, n))
    qb = np.quantile(_sample(color.luma_display(b_display), None), np.linspace(0, 1, n))
    return float(np.mean(np.abs(qa - qb)))


def rank_tonemaps(lin: np.ndarray, ref_display: Img, candidates: list[str] | None = None) -> list[dict]:
    """リニア画像 lin を、表示参照の基準(ref_display。UE のスクショ等)へ最も近づけるトーンマップと EV を探す。

    各候補で「中央輝度が一致する EV」を二分法で求め、残る分布差(輝度分位の EMD)で並べる。
    小さいほど良い。グレーカード等が無い前提のヒストグラム合わせなので、EV は絶対値ではなく
    「その候補で分布の中心を合わせたらこうなる」という参考値。
    """
    cands = candidates or ["engine_aces", "aces_narkowicz", "aces_hill", "linear_clip", "reinhard", "engine_agx"]
    target = float(np.median(_sample(color.luma_display(ref_display.rgb), None)))
    out = []
    for name in cands:
        ev = fit_ev_to_display(lin, name, target)
        d, e = apply_tonemap(name, apply_ev(lin, ev))
        out.append({"tonemap": name, "ev": round(ev, 3), "emd": luma_quantile_emd(d, ref_display.rgb),
                    "note": get_tonemap(name).note})
    out.sort(key=lambda r: r["emd"])
    return out
