"""PS-0(校正シーン)の生成器。使い捨てプロジェクトを作る(実プロジェクトには触らない)。

  python ps0_calibration.py --out <dir> [--id ps0_calibration] [--args-json '{"variant":"calibration","units":"physical"}']

作るもの(プリミティブだけ。外部アセット無し):
  variant = calibration(既定)
    * 18% グレーカード(リニア反射率 0.18)・グレースケールのランプ(11 段)・24 色パッチ(ColorChecker の sRGB 値をリニアへ変換)
    * チェッカー床(2 色の箱を市松に並べる)
    * ローネス x メタルネス 5x5 の球の格子(BSDF 校正)
    * 太陽 1 つ + 手続きの空(IBL)
    * 白い炉(furnace)用の白球 1 つ(一様な白い環境が要る。環境の設定はエンジン側の機能が揃ってから setup に足す)
  variant = lightsteps  … 光源強度の段階。灰色の壁の前に点光源を 4 灯(光度 x1 / x4 / x16 / x64)。太陽・空は無し(直接光だけ)
  variant = invsq       … 逆二乗の距離テスト。長い床の端の近くに点光源 1 灯。1〜10 m で明るさが 2 桁落ちる(太陽・空は無し)

units:
  legacy(既定)  … 従来どおり(強度は任意単位。太陽 3.0 など)。既定の絵は 1 ビットも変えない。
  physical      … 物理ライティング単位(シーン設定 lightingUnits=1)。太陽 = lux / 点光源 = cd / 空 = nit。
                  露出は EV100(手動)、トーンマップは UE Filmic。postProcess はシーン JSON に埋めるので setup は不要。

★色の意味(頂点色 = 基本色の乗算。リニア値として書いている)は PS-0 の実測(グレーカードの画素値)で確かめること。
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

CHECKER_SRGB = [
    (115, 82, 68), (194, 150, 130), (98, 122, 157), (87, 108, 67), (133, 128, 177), (103, 189, 170),
    (214, 126, 44), (80, 91, 166), (193, 90, 99), (94, 60, 108), (157, 188, 64), (224, 163, 46),
    (56, 61, 150), (70, 148, 73), (175, 54, 60), (231, 199, 31), (187, 86, 149), (8, 133, 161),
    (243, 243, 242), (200, 200, 200), (160, 160, 160), (122, 122, 121), (85, 85, 85), (52, 52, 52),
]
RAMP_ALBEDO = [0.0, 0.02, 0.05, 0.1, 0.18, 0.3, 0.45, 0.6, 0.75, 0.9, 1.0]

# 物理モードの既定値(Sunny 16: 太陽 8 万 lux・空 8000 nit・EV100=15)
PHYS_DEFAULTS = {"sun_lux": 80000.0, "sky_nits": 8000.0, "ev100": 15.0}


def _lin(c: int) -> float:
    v = c / 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def _guid(name: str) -> str:
    return hashlib.md5(name.encode("utf-8")).hexdigest()[:16]


def _ent(name: str, prim: str, pos, scale, color, metallic=0.0, roughness=0.9, size: float | None = None) -> dict:
    e = {"guid": _guid(name), "name": name, "primitive": prim, "color": [round(c, 6) for c in color],
         "material": {"metallic": metallic, "roughness": roughness},
         "transform": {"position": list(pos), "rotation": [0.0, 0.0, 0.0], "scale": list(scale)}}
    if size is not None:
        e["primitiveSize"] = size
    return e


def _sun(intensity: float, ambient: float) -> dict:
    return {"guid": _guid("Sun"), "name": "Sun",
            "directionalLight": {"ambient": ambient, "color": [1.0, 1.0, 1.0], "direction": [0.35, -0.55, 0.76], "intensity": intensity},
            "transform": {"position": [0.0, 10.0, 0.0], "rotation": [-33.0, 25.0, 0.0], "scale": [1.0, 1.0, 1.0]}}


def _point(name: str, pos, candela: float, rng: float = 500.0, radius: float = 0.0) -> dict:
    """点光源。physical では intensity = 光度[cd]。range は影響半径の窓(saturate(1-(d/R)^4)^2)が効かないよう十分大きくする。"""
    return {"guid": _guid(name), "name": name,
            "pointLight": {"color": [1.0, 1.0, 1.0], "intensity": candela, "range": rng, "castShadows": False, "sourceRadius": radius},
            "transform": {"position": list(pos), "rotation": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]}}


def _base_scene(entities: list[dict], *, physical: bool, sky_nits: float, ev100: float, ibl: bool, draw_sky: bool,
                tonemapper: int = 3) -> dict:
    """校正用の共通設定: スクリーン空間効果・TAA は全部 OFF。physical なら postProcess に物理単位 + 手動 EV100 + UE Filmic。"""
    s = {"version": 1, "shadows": True, "entities": entities,
         "ssao": {"enabled": False}, "contactShadow": {"enabled": False}, "ssr": {"enabled": False},
         "ssgi": {"enabled": False}, "taa": {"enabled": False}}
    if ibl:
        s["skybox"] = {"drawSkybox": draw_sky, "envMapPath": "__procedural_sky__",
                       "iblIntensity": sky_nits if physical else 1.0, "skyboxIntensity": sky_nits if physical else 1.0}
    else:
        s["skybox"] = {"drawSkybox": False, "envMapPath": "", "iblIntensity": 0.0, "skyboxIntensity": 0.0}
    if physical:
        # マスター(enabled)を切る: グレーディング系の効果は全部 OFF のまま、露出とトーンマップだけが効く(露出モード 1/2 はマスターに依らない)。
        s["postProcess"] = {"enabled": False, "lightingUnits": 1, "exposureMode": 1, "ev100": ev100, "evComp": 0.0,
                            "tonemapper": tonemapper, "debandOn": False}
    else:
        s["postProcess"] = {"enabled": False, "debandOn": False}
    return s


def build_calibration(args: dict) -> dict:
    physical = args.get("units", "legacy") == "physical"
    sun = float(args.get("sun_intensity", PHYS_DEFAULTS["sun_lux"] if physical else 3.0))
    sky = float(args.get("sky_nits", PHYS_DEFAULTS["sky_nits"]))
    ents: list[dict] = [_sun(sun, 0.0 if physical else 0.25)]
    # チェッカー床(8x8 マス、1m)
    for i in range(8):
        for j in range(8):
            v = 0.5 if (i + j) % 2 == 0 else 0.15
            ents.append(_ent(f"Floor_{i}_{j}", "box", (i - 3.5, -0.05, j - 3.5 + 4.0), (1.0, 0.1, 1.0), (v, v, v)))
    # グレーカード(18%)
    ents.append(_ent("GrayCard", "box", (-2.6, 0.5, 2.0), (0.9, 0.9, 0.03), (0.18, 0.18, 0.18)))
    # 24 パッチ(4 行 x 6 列)
    for k, (r, g, b) in enumerate(CHECKER_SRGB):
        row, col = divmod(k, 6)
        ents.append(_ent(f"Patch_{k + 1:02d}", "box", (-1.5 + col * 0.35, 1.25 - row * 0.35, 2.0), (0.3, 0.3, 0.03), (_lin(r), _lin(g), _lin(b))))
    # グレースケールのランプ(11 段。反射率 0 → 1)
    for k, a in enumerate(RAMP_ALBEDO):
        ents.append(_ent(f"Ramp_{k:02d}", "box", (-1.5 + k * 0.19, 1.7, 2.0), (0.17, 0.3, 0.03), (a, a, a)))
    # 球の格子: 列 = ローネス 0.05..1、行 = メタルネス 0..1
    for mi, metallic in enumerate([0.0, 0.25, 0.5, 0.75, 1.0]):
        for ri, rough in enumerate([0.05, 0.25, 0.5, 0.75, 1.0]):
            ents.append(_ent(f"Sphere_m{mi}_r{ri}", "sphere", (1.0 + ri * 0.7, 0.3 + mi * 0.7, 2.0), (0.6, 0.6, 0.6), (0.8, 0.8, 0.8),
                             metallic=metallic, roughness=rough, size=0.5))
    # 白い炉用の白球
    ents.append(_ent("FurnaceSphere", "sphere", (0.0, 0.8, 6.0), (1.2, 1.2, 1.2), (1.0, 1.0, 1.0), roughness=1.0, size=0.5))
    return _base_scene(ents, physical=physical, sky_nits=sky, ev100=float(args.get("ev100", PHYS_DEFAULTS["ev100"])),
                       ibl=True, draw_sky=True)


def build_lightsteps(args: dict) -> dict:
    """光源強度の段階: 灰色の壁(反射率 0.5)の前に点光源 4 灯。光度 = base x {1,4,16,64}。"""
    physical = args.get("units", "legacy") == "physical"
    base = float(args.get("base_cd", 100.0 if physical else 0.25))
    ents: list[dict] = [_sun(0.0, 0.0)]
    ents.append(_ent("Wall", "box", (0.0, 1.5, 4.0), (10.0, 4.0, 0.1), (0.5, 0.5, 0.5), roughness=1.0))
    ents.append(_ent("Floor", "box", (0.0, -0.05, 1.0), (10.0, 0.1, 8.0), (0.35, 0.35, 0.35), roughness=1.0))
    for i, mult in enumerate((1.0, 4.0, 16.0, 64.0)):
        ents.append(_point(f"Light_x{int(mult)}", (-3.0 + 2.0 * i, 1.5, 2.6), base * mult, rng=200.0, radius=0.02))
    return _base_scene(ents, physical=physical, sky_nits=0.0, ev100=float(args.get("ev100", 6.0)), ibl=False, draw_sky=False)


def build_invsq(args: dict) -> dict:
    """逆二乗の距離テスト: 長い灰色の床の端の近くの点光源 1 灯(高さ 1 m)。床の上で 1〜10 m の距離が一度に見える。"""
    physical = args.get("units", "legacy") == "physical"
    cd = float(args.get("cd", 400.0 if physical else 0.5))
    ents: list[dict] = [_sun(0.0, 0.0)]
    ents.append(_ent("Floor", "box", (0.0, -0.05, 4.0), (6.0, 0.1, 16.0), (0.5, 0.5, 0.5), roughness=1.0))
    ents.append(_point("Light", (0.0, 1.0, -3.5), cd, rng=500.0, radius=0.02))
    return _base_scene(ents, physical=physical, sky_nits=0.0, ev100=float(args.get("ev100", 5.0)), ibl=False, draw_sky=False)


VARIANTS = {"calibration": build_calibration, "lightsteps": build_lightsteps, "invsq": build_invsq}


def build_scene(sun_intensity: float = 3.0, **args) -> dict:
    """後方互換: build_scene(sun_intensity) は従来の legacy calibration。"""
    a = dict(args)
    a.setdefault("sun_intensity", sun_intensity)
    return VARIANTS[a.get("variant", "calibration")](a)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--id", default="ps0_calibration")
    ap.add_argument("--args-json", default="{}")
    a = ap.parse_args()
    args = json.loads(a.args_json or "{}")
    variant = args.get("variant", "calibration")
    if variant not in VARIANTS:
        raise SystemExit(f"variant が不明: {variant}(候補: {', '.join(VARIANTS)})")
    out = Path(a.out)
    (out / "assets" / "scenes").mkdir(parents=True, exist_ok=True)
    (out / "scripts").mkdir(exist_ok=True)
    scene_rel = f"scenes/{a.id}.json"
    (out / "assets" / "scenes" / f"{a.id}.json").write_text(json.dumps(VARIANTS[variant](args), indent=2), encoding="utf-8")
    (out / "ParityGen.dx12proj").write_text(json.dumps(
        {"assetsDir": "assets", "defaultScene": scene_rel, "lastOpenedScene": scene_rel, "name": "ParityGen",
         "scriptsDir": "scripts", "version": "0.1.0"}, indent=2), encoding="utf-8")
    print(json.dumps({"project": str(out), "scene": scene_rel}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
