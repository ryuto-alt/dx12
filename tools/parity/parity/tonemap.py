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
  ue_filmic_approx UE 既定の Filmic は ACES 準拠に設計されているので aces_hill の別名(近似。Q2 で ue_filmic が入ったが旧来の名前のまま残す)
  ue_filmic        UE 5 の Filmic(ACES 系。Slope 0.88 / Toe 0.55 / Shoulder 0.26 / BlackClip 0 / WhiteClip 0.04 を記憶から独自に書き起こし)+ sRGB
                   = Uno のトーンマッパ 3(engine 側 shaders/post/Tonemap.hlsli)と同じ式。根拠と不確実性は src/renderer/PhotometricMath.h
  pbr_neutral      Khronos PBR Neutral + sRGB = Uno のトーンマッパ 5
  reinhard         x/(1+x) → sRGB
  engine_agx       Uno の AgX(Wrensch フィット。PostProcess.hlsl と同じ係数)  (eotf=gamma22)

Uno のトーンマッパ番号(PostProcessSettings::tonemapper)との対応: 0=engine_aces 1=engine_agx 2=(ガンマのみ) 3=ue_filmic 4=linear_clip 5=pbr_neutral
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


# ── Q2: UE 5 の Filmic と Khronos PBR Neutral(engine の Tonemap.hlsli / PhotometricMath.h と同じ式。numpy 版=独立実装) ──

_SRGB_AP0 = np.array([[0.4396329819, 0.3829886982, 0.1773783199],
                      [0.0897764430, 0.8134394287, 0.0967841283],
                      [0.0175411704, 0.1115465533, 0.8709122763]], dtype=np.float64)
_AP0_AP1 = np.array([[1.4514393161, -0.2365107469, -0.2149285693],
                     [-0.0765537734, 1.1762296998, -0.0996759264],
                     [0.0083161484, -0.0060324498, 0.9977163014]], dtype=np.float64)
_AP1_SRGB = np.array([[1.7050509927, -0.6217921207, -0.0832588720],
                      [-0.1302564175, 1.1408047366, -0.0105483191],
                      [-0.0240033568, -0.1289689761, 1.1529723329]], dtype=np.float64)
_AP1_Y = np.array([0.2722287168, 0.6740817658, 0.0536895174], dtype=np.float64)

UE_FILM_DEFAULT = {"slope": 0.88, "toe": 0.55, "shoulder": 0.26, "black_clip": 0.0, "white_clip": 0.04}


def ue_filmic_linear(rgb, slope=0.88, toe=0.55, shoulder=0.26, black_clip=0.0, white_clip=0.04) -> np.ndarray:
    """UE 5 の既定 Filmic(ACES 系)。入力=リニア sRGB(露出済み)。出力=リニア sRGB(1 を超えうる。クリップは呼び出し側)。"""
    x = np.asarray(rgb, dtype=np.float64)
    aces = x @ _SRGB_AP0.T
    mi, ma = aces.min(-1), aces.max(-1)
    sat = (np.maximum(ma, 1e-10) - np.maximum(mi, 1e-10)) / np.maximum(ma, 1e-2)
    r, g, b = aces[..., 0], aces[..., 1], aces[..., 2]
    yc = (b + g + r + 1.75 * np.sqrt(np.maximum(b * (b - g) + g * (g - r) + r * (r - b), 0.0))) / 3.0
    sx = (sat - 0.4) / 0.2
    t0 = np.maximum(1.0 - np.abs(sx / 2.0), 0.0)
    s_shaper = (1.0 + np.sign(sx) * (1.0 - t0 * t0)) / 2.0
    gain, mid = 0.05 * s_shaper, 0.08
    glow = np.where(yc <= (2.0 / 3.0) * mid, gain, np.where(yc >= 2.0 * mid, 0.0, gain * (mid / np.maximum(yc, 1e-10) - 0.5)))
    aces = aces * (1.0 + glow)[..., None]
    r, g, b = aces[..., 0], aces[..., 1], aces[..., 2]
    hue = np.degrees(np.arctan2(np.sqrt(3.0) * (g - b), 2 * r - g - b))
    hue = np.where(hue < 0, hue + 360.0, hue)
    hue = np.where((r == g) & (g == b), 0.0, hue)
    centered = np.where(hue < -180.0, hue + 360.0, np.where(hue > 180.0, hue - 360.0, hue))
    u = np.clip(1.0 - np.abs(2.0 * centered / 135.0), 0.0, 1.0)
    hue_w = (u * u * (3.0 - 2.0 * u)) ** 2
    aces = aces.copy()
    aces[..., 0] = aces[..., 0] + hue_w * sat * (0.03 - aces[..., 0]) * (1.0 - 0.82)
    pre = np.maximum(aces @ _AP0_AP1.T, 0.0)
    py = pre @ _AP1_Y
    pre = py[..., None] + (pre - py[..., None]) * 0.96

    toe_scale = 1.0 + black_clip - toe
    sh_scale = 1.0 + white_clip - shoulder
    if toe > 0.8:
        toe_match = (1.0 - toe - 0.18) / slope + np.log10(0.18)
    else:
        bt = (0.18 + black_clip) / toe_scale - 1.0
        toe_match = np.log10(0.18) - 0.5 * np.log((1.0 + bt) / (1.0 - bt)) * (toe_scale / slope)
    straight_match = (1.0 - toe) / slope - toe_match
    shoulder_match = shoulder / slope - straight_match

    lc = np.log10(np.maximum(pre, 1e-30))
    straight = slope * (lc + straight_match)
    toe_c = -black_clip + (2.0 * toe_scale) / (1.0 + np.exp((-2.0 * slope / toe_scale) * (lc - toe_match)))
    sh_c = (1.0 + white_clip) - (2.0 * sh_scale) / (1.0 + np.exp((2.0 * slope / sh_scale) * (lc - shoulder_match)))
    toe_c = np.where(lc < toe_match, toe_c, straight)
    sh_c = np.where(lc > shoulder_match, sh_c, straight)
    t = np.clip((lc - toe_match) / (shoulder_match - toe_match), 0.0, 1.0)
    if shoulder_match < toe_match:
        t = 1.0 - t
    t = (3.0 - 2.0 * t) * t * t
    tone = toe_c + (sh_c - toe_c) * t
    ty = tone @ _AP1_Y
    tone = np.maximum(ty[..., None] + (tone - ty[..., None]) * 0.93, 0.0)
    return tone @ _AP1_SRGB.T


def pbr_neutral_linear(c) -> np.ndarray:
    """Khronos PBR Neutral(リニア → リニア。クリップ前)。"""
    c = np.maximum(np.asarray(c, dtype=np.float64), 0.0)
    start, desat = 0.8 - 0.04, 0.15
    x = c.min(-1)
    offset = np.where(x < 0.08, x - 6.25 * x * x, 0.04)
    c = c - offset[..., None]
    peak = c.max(-1)
    d = 1.0 - start
    new_peak = 1.0 - d * d / (peak + d - start)
    scaled = c * (new_peak / np.maximum(peak, 1e-10))[..., None]
    g = 1.0 - 1.0 / (desat * (peak - new_peak) + 1.0)
    comp = scaled + (new_peak[..., None] - scaled) * g[..., None]
    return np.where((peak < start)[..., None], c, comp)


def ev100_to_scale(ev100: float, ev_comp: float = 0.0) -> float:
    """手動露出の係数 F = 1/(1.2·2^(EV100 - 補正))。Uno の PhotometricMath.h ManualExposureScale と同じ。"""
    return 1.0 / (1.2 * 2.0 ** (ev100 - ev_comp))


def ev100_to_ev(ev100: float, ev_comp: float = 0.0) -> float:
    """露出係数を log2 で(alignment.exposure の ev_ref / ev_test にそのまま使える EV)。"""
    return float(np.log2(ev100_to_scale(ev100, ev_comp)))


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
    # Q2: Uno の新しいトーンマッパ(3=UE Filmic / 4=線形 / 5=PBR Neutral)。出力は sRGB OETF。
    t["ue_filmic"] = Tonemap("ue_filmic", lambda x: np.clip(ue_filmic_linear(x), 0.0, 1.0).astype(np.float32), "srgb",
                             "UE 5 の Filmic(Uno のトーンマッパ 3)+ sRGB")
    t["engine_ue_filmic"] = t["ue_filmic"]
    t["pbr_neutral"] = Tonemap("pbr_neutral", lambda x: np.clip(pbr_neutral_linear(x), 0.0, 1.0).astype(np.float32), "srgb",
                               "Khronos PBR Neutral(Uno のトーンマッパ 5)+ sRGB")
    t["engine_pbr_neutral"] = t["pbr_neutral"]
    t["engine_linear"] = t["linear_clip"]
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
        if s.startswith("ev100:"):
            # Q2: 物理露出。ev100:15 / ev100:15,+1。線形の両者(基準とエンジン)に同じ係数 F = 1/(1.2·2^(EV100-補正)) を掛ける
            parts = s.split(":", 1)[1].split(",")
            ev = ev100_to_ev(float(parts[0]), float(parts[1]) if len(parts) > 1 else 0.0)
            return {"mode": "fixed_ev", "ev_ref": ev, "ev_test": ev}
        raise ValueError(f"露出の指定が不明: {spec}(none / auto / fixed:EV / ev100:EV100[,補正])")
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
    cands = candidates or ["engine_aces", "aces_narkowicz", "aces_hill", "ue_filmic", "pbr_neutral", "linear_clip", "reinhard", "engine_agx"]
    target = float(np.median(_sample(color.luma_display(ref_display.rgb), None)))
    out = []
    for name in cands:
        ev = fit_ev_to_display(lin, name, target)
        d, e = apply_tonemap(name, apply_ev(lin, ev))
        out.append({"tonemap": name, "ev": round(ev, 3), "emd": luma_quantile_emd(d, ref_display.rgb),
                    "note": get_tonemap(name).note})
    out.sort(key=lambda r: r["emd"])
    return out
