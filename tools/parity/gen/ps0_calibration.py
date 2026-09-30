"""PS-0(校正シーン)の生成器。使い捨てプロジェクトを作る(実プロジェクトには触らない)。

  python ps0_calibration.py --out <dir> [--id ps0_calibration] [--args-json '{"sun_intensity":3.0}']

作るもの(プリミティブだけ。外部アセット無し):
  * 18% グレーカード(リニア反射率 0.18)と 24 色パッチ(ColorChecker の sRGB 値をリニアへ変換)
  * チェッカー床(2 色の箱を市松に並べる)
  * ローネス x メタルネス 5x5 の球の格子(BSDF 校正)
  * 太陽 1 つ(強度は args)
  * 白い炉(furnace)用の白球 1 つ(一様な白い環境が要る。環境の設定はエンジン側の機能が揃ってから setup に足す)

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


def build_scene(sun_intensity: float = 3.0) -> dict:
    ents: list[dict] = []
    ents.append({"guid": _guid("Sun"), "name": "Sun",
                 "directionalLight": {"ambient": 0.25, "color": [1.0, 1.0, 1.0], "direction": [0.35, -0.55, 0.76], "intensity": sun_intensity},
                 "transform": {"position": [0.0, 10.0, 0.0], "rotation": [-33.0, 25.0, 0.0], "scale": [1.0, 1.0, 1.0]}})
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
    # 球の格子: 列 = ローネス 0.05..1、行 = メタルネス 0..1
    for mi, metallic in enumerate([0.0, 0.25, 0.5, 0.75, 1.0]):
        for ri, rough in enumerate([0.05, 0.25, 0.5, 0.75, 1.0]):
            ents.append(_ent(f"Sphere_m{mi}_r{ri}", "sphere", (1.0 + ri * 0.7, 0.3 + mi * 0.7, 2.0), (0.6, 0.6, 0.6), (0.8, 0.8, 0.8),
                             metallic=metallic, roughness=rough, size=0.5))
    # 白い炉用の白球
    ents.append(_ent("FurnaceSphere", "sphere", (0.0, 0.8, 6.0), (1.2, 1.2, 1.2), (1.0, 1.0, 1.0), roughness=1.0, size=0.5))
    return {"version": 1, "shadows": True, "entities": ents,
            "skybox": {"drawSkybox": True, "envMapPath": "__procedural_sky__", "iblIntensity": 1.0, "skyboxIntensity": 1.0}}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--id", default="ps0_calibration")
    ap.add_argument("--args-json", default="{}")
    a = ap.parse_args()
    args = json.loads(a.args_json or "{}")
    out = Path(a.out)
    (out / "assets" / "scenes").mkdir(parents=True, exist_ok=True)
    (out / "scripts").mkdir(exist_ok=True)
    scene_rel = f"scenes/{a.id}.json"
    (out / "assets" / "scenes" / f"{a.id}.json").write_text(
        json.dumps(build_scene(float(args.get("sun_intensity", 3.0))), indent=2), encoding="utf-8")
    (out / "ParityGen.dx12proj").write_text(json.dumps(
        {"assetsDir": "assets", "defaultScene": scene_rel, "lastOpenedScene": scene_rel, "name": "ParityGen",
         "scriptsDir": "scripts", "version": "0.1.0"}, indent=2), encoding="utf-8")
    print(json.dumps({"project": str(out), "scene": scene_rel}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
