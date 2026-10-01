"""GI 評価ハーネス(S1)の検証シーン 4 本(GR-1〜GR-4)を tools/parity/scenes/gi/*.json へ書き出す。

  python gi_scenes.py [--out <dir>]      # 既定: tools/parity/scenes/gi

各ファイルの形(1 ファイル = 1 シーン):
  id / title / description
  spec      … SceneSpec v1(`dx12_apply_scene_spec {spec}` にそのまま渡せる。自作の箱・球・光だけ。verify:false = 壁の角が重なるため)
  sun       … `dx12_set_sun` の引数(azimuth / elevation / intensity / ambient=0 / kelvin)
  ddgi      … DDGI の格子を「このバウンディングボックスを覆う」ように自動で決めるための指定({min, max, spacing})
  cameras[] … {name, position, target, displayEv?}。垂直 FOV は 45°(エディタカメラ固定)
  regions   … 領域マスクの定義(エンティティ名の集合。必要なら nearEntity / maxDist)。マスクは解析的な光線投射で自動生成する(手描きなし)
  criteria  … このシーンで判定する合否(A1〜A4。information は参考表示)

★既定シーンの "Sun" と "Grid" は入れない(ランナーは使い捨てプロジェクトに自分のエンティティだけを書く)。
  MCP で手動適用するときは dx12_new_scene の後に Sun / Grid を消してから spec を適用すること(Sun が残ると最初の平行光として使われる)。
★定数 ambient は 0(パストレーサーは定数 ambient を持たない)。
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

GREY = "#d8d4cc"
GREEN = "#3f8a4a"
FLOOR_BROWN = "#8a6a4c"
RED = "#c02a22"


def box(name, at, size, color, rough=0.9, metallic=0.0, group="LVL", emissive=None, emissive_intensity=None):
    mat = {"roughness": rough}
    if metallic:
        mat["metallic"] = metallic
    if emissive is not None:
        mat["emissive"] = emissive
        mat["emissiveIntensity"] = emissive_intensity if emissive_intensity is not None else 1.0
    return {"name": name, "kind": "box", "group": group, "at": list(at), "size": list(size), "color": color, "material": mat}


def sphere(name, at, diameter, color, rough=0.5, metallic=0.0, group="ENV"):
    mat = {"roughness": rough}
    if metallic:
        mat["metallic"] = metallic
    return {"name": name, "kind": "sphere", "group": group, "at": list(at), "size": diameter, "color": color, "material": mat}


def point(name, at, intensity, rng, color, cast=True):
    return {"name": name, "kind": "light", "light": "point", "group": "LGT", "at": list(at),
            "components": {"pointLight": {"intensity": intensity, "range": rng, "color": list(color), "castShadows": cast}}}


def sun_entity():
    return {"name": "LGT_Sun", "kind": "light", "light": "directional", "group": "LGT", "at": [0, 20, 0], "rotation": [-33, 90, 0],
            "components": {"directionalLight": {"intensity": 3.0, "ambient": 0.0}}}


def window_wall_west(prefix, x, h, z_half, win_z, win_y, thick=0.2, color=GREY):
    """西壁(x 固定)を窓の開口 win_z=[z0,z1]・win_y=[y0,y1] を空けた 4 枚で作る。壁は y∈[0,h]・z∈[-z_half,z_half]。"""
    z0, z1 = win_z
    y0, y1 = win_y
    out = []
    out.append(box(f"{prefix}_Lo", [x, y0 / 2, 0], [thick, y0, 2 * z_half], color))                          # 窓の下
    out.append(box(f"{prefix}_Hi", [x, (y1 + h) / 2, 0], [thick, round(h - y1, 4), 2 * z_half], color))                # 窓の上
    out.append(box(f"{prefix}_L", [x, (y0 + y1) / 2, (-z_half + z0) / 2], [thick, y1 - y0, z0 + z_half], color))   # 窓の手前側(-z)
    out.append(box(f"{prefix}_R", [x, (y0 + y1) / 2, (z1 + z_half) / 2], [thick, y1 - y0, z_half - z1], color))    # 窓の奥側(+z)
    return out


def gr1() -> dict:
    ents = [
        box("LVL_Floor", [0, -0.1, 0], [8.4, 0.2, 6.4], FLOOR_BROWN, rough=0.35),
        box("LVL_Ceiling", [0, 3.3, 0], [8.4, 0.2, 6.4], GREY),
        box("LVL_Wall_N", [0, 1.6, 3.1], [8.4, 3.2, 0.2], GREY),
        box("LVL_Wall_S", [0, 1.6, -3.1], [8.4, 3.2, 0.2], GREY),
        box("LVL_Wall_E", [4.1, 1.6, 0], [0.2, 3.2, 6.4], GREEN),
        *window_wall_west("LVL_Wall_W", -4.1, 3.2, 3.2, [-1.0, 1.0], [1.0, 2.4]),
        box("LVL_Ground", [0, -0.45, 0], [80, 0.2, 80], "#6e6a5e", rough=0.95),
        box("ENV_Rug", [-1.7, 0.015, 0], [2.2, 0.03, 2.2], RED),
        sphere("ENV_SphereGold", [1.0, 0.4, 1.4], 0.8, "#e8b048", rough=0.2, metallic=1.0),
        sphere("ENV_SphereChrome", [2.2, 0.35, 0.0], 0.7, "#ffffff", rough=0.03, metallic=1.0),
        sphere("ENV_SphereWhite", [0.6, 0.35, -1.0], 0.7, "#ffffff", rough=0.8),
        sphere("ENV_SphereBlue", [-0.6, 0.3, 2.1], 0.6, "#2a5ad0", rough=0.15),
        sun_entity(),
    ]
    walls = ["LVL_Wall_N", "LVL_Wall_S", "LVL_Wall_E", "LVL_Wall_W_Lo", "LVL_Wall_W_Hi", "LVL_Wall_W_L", "LVL_Wall_W_R"]
    return {
        "id": "gr1",
        "title": "GR-1 小窓の部屋",
        "description": "8.4x3.2x6.4m・西壁に 2x1.4m の窓・太陽 方位 270°/高度 33°・赤い敷物・金/クロム/白/青の球。空の遮蔽・色の照り返し・金属球の環境反射を見る。",
        "spec": {"version": 1, "name": "gr1_small_window", "verify": False, "entities": ents},
        "sun": {"azimuth": 270, "elevation": 33, "intensity": 3, "ambient": 0, "kelvin": 5600},
        "ddgi": {"min": [-4.0, 0.0, -3.0], "max": [4.0, 3.2, 3.0], "spacing": 0.8},
        "cameras": [
            {"name": "main", "position": [3.3, 1.7, -2.7], "target": [-2, 0.9, 1]},
            {"name": "ceiling", "position": [3.3, 1.2, -2.7], "target": [-1.5, 2.2, 1.5],
             "note": "天井の R/G 比(A3)用の補助カメラ。主カメラは天井が画面の端にしか写らない"},
        ],
        "regions": {
            "walls": {"entities": walls},
            "ceiling": {"entities": ["LVL_Ceiling"]},
            "floor": {"entities": ["LVL_Floor"]},
            "floor_near_rug": {"entities": ["LVL_Floor"], "nearEntity": "ENV_Rug", "maxDist": 0.9},
            "rug": {"entities": ["ENV_Rug"]},
            "sphere_gold": {"entities": ["ENV_SphereGold"]},
            "sphere_chrome": {"entities": ["ENV_SphereChrome"]},
            "sphere_white": {"entities": ["ENV_SphereWhite"]},
            "sphere_blue": {"entities": ["ENV_SphereBlue"]},
            "outside": {"entities": ["LVL_Ground"]},
        },
        "criteria": {"A1": {"regions": ["walls", "ceiling", "floor"], "cameras": ["main", "ceiling"]},
                     "A3": {"region": "ceiling", "camera": "ceiling"},
                     "A4": {"region": "sphere_chrome", "camera": "main"}},
    }


def gr2() -> dict:
    ents = [
        box("LVL_Floor", [0, -0.1, 0], [6.4, 0.2, 6.4], FLOOR_BROWN, rough=0.35),
        box("LVL_Ceiling", [0, 3.1, 0], [6.4, 0.2, 6.4], GREY),
        box("LVL_Wall_N", [0, 1.5, 3.1], [6.4, 3.0, 0.2], GREY),
        box("LVL_Wall_S", [0, 1.5, -3.1], [6.4, 3.0, 0.2], GREY),
        box("LVL_Wall_E", [3.1, 1.5, 0], [0.2, 3.0, 6.0], GREY),
        box("LVL_Wall_W", [-3.1, 1.5, 0], [0.2, 3.0, 6.0], GREY),
        box("ENV_BoxRed", [-1.5, 0.5, 0.8], [1.0, 1.0, 1.0], RED),
        box("ENV_BoxWhite", [1.4, 0.4, -1.0], [0.8, 0.8, 0.8], "#f0f0f0"),
        sphere("ENV_SphereChrome", [1.0, 0.35, 1.6], 0.7, "#ffffff", rough=0.03, metallic=1.0),
        sphere("ENV_SphereGold", [-0.2, 0.4, -1.8], 0.8, "#e8b048", rough=0.2, metallic=1.0),
        point("LGT_Bulb", [0, 2.7, 0], 8.0, 8.0, [1.0, 0.9, 0.75]),
        sun_entity(),
    ]
    walls = ["LVL_Wall_N", "LVL_Wall_S", "LVL_Wall_E", "LVL_Wall_W"]
    return {
        "id": "gr2",
        "title": "GR-2 窓の無い部屋 + 天井の点光源",
        "description": "6x3x6m の密閉した部屋。天井中央に点光源 1 灯だけ。空(IBL・太陽)の寄与は 0 であるべき(太陽は強度 0)。",
        "spec": {"version": 1, "name": "gr2_closed_room", "verify": False, "entities": ents},
        "sun": {"azimuth": 270, "elevation": 33, "intensity": 0, "ambient": 0, "kelvin": 5600},
        "ddgi": {"min": [-3.0, 0.0, -3.0], "max": [3.0, 3.0, 3.0], "spacing": 0.8},
        "cameras": [{"name": "main", "position": [-2.7, 1.6, -2.7], "target": [1.0, 1.0, 1.5]}],
        "regions": {
            "walls": {"entities": walls},
            "ceiling": {"entities": ["LVL_Ceiling"]},
            "floor": {"entities": ["LVL_Floor"]},
            "box_red": {"entities": ["ENV_BoxRed"]},
            "box_white": {"entities": ["ENV_BoxWhite"]},
            "sphere_chrome": {"entities": ["ENV_SphereChrome"]},
            "sphere_gold": {"entities": ["ENV_SphereGold"]},
        },
        "criteria": {"information": {"regions": ["walls", "ceiling", "floor", "box_red", "box_white"], "cameras": ["main"],
                                     "note": "空の寄与が 0 の密閉室。全領域 ±0.5EV が目安(情報のみ。A1〜A4 には数えない)"}},
    }


def gr3() -> dict:
    ents = [
        box("LVL_Floor", [0, -0.1, 0], [8.4, 0.2, 6.4], FLOOR_BROWN, rough=0.35),
        box("LVL_Ceiling", [0, 3.1, 0], [8.4, 0.2, 6.4], GREY),
        box("LVL_Wall_N", [0, 1.5, 3.1], [8.4, 3.0, 0.2], GREY),
        box("LVL_Wall_S", [0, 1.5, -3.1], [8.4, 3.0, 0.2], GREY),
        box("LVL_Wall_E", [4.1, 1.5, 0], [0.2, 3.0, 6.0], GREY),
        *window_wall_west("LVL_Wall_W", -4.1, 3.0, 3.0, [-1.2, 1.2], [1.0, 2.4]),
        box("LVL_Partition", [0, 1.5, 0], [0.1, 3.0, 6.0], GREY),                          # 厚さ 0.1m の仕切り(x=0)
        box("LVL_Ground", [0, -0.45, 0], [80, 0.2, 80], "#6e6a5e", rough=0.95),
        box("ENV_RugA", [-1.5, 0.015, 0], [2.2, 0.03, 2.2], RED),
        box("ENV_CrateB", [2.4, 0.4, 0.6], [0.8, 0.8, 0.8], "#f0f0f0"),
        sun_entity(),
    ]
    dark_walls = ["LVL_Wall_N", "LVL_Wall_S", "LVL_Wall_E", "LVL_Partition"]
    return {
        "id": "gr3",
        "title": "GR-3 薄い仕切り壁を挟んだ明暗 2 部屋",
        "description": "x<0 の部屋だけ西の窓から太陽が入り、x>0 の部屋は密閉(厚さ 0.1m の仕切り)。暗い側に光が漏れてはいけない(ddgi_leak 相当)。",
        "spec": {"version": 1, "name": "gr3_partition", "verify": False, "entities": ents},
        "sun": {"azimuth": 270, "elevation": 33, "intensity": 3, "ambient": 0, "kelvin": 5600},
        "ddgi": {"min": [-4.0, 0.0, -3.0], "max": [4.0, 3.0, 3.0], "spacing": 0.8},
        "cameras": [{"name": "dark", "position": [3.5, 1.6, -2.5], "target": [0.0, 1.0, 0.8],
                     "displayEv": 3.0, "note": "暗い側(x>0)から仕切り壁と床を見る。PT はほぼ真っ黒なので表示用に +3EV"}],
        "regions": {
            "dark_walls": {"entities": dark_walls},
            "dark_floor": {"entities": ["LVL_Floor"]},
            "dark_ceiling": {"entities": ["LVL_Ceiling"]},
            "dark_crate": {"entities": ["ENV_CrateB"]},
            "dark_all": {"entities": dark_walls + ["LVL_Floor", "LVL_Ceiling", "ENV_CrateB"]},
        },
        "criteria": {"A2": {"region": "dark_all", "camera": "dark"}},
    }


def gr4() -> dict:
    L = 20.0
    ents = [
        box("LVL_Floor", [0, -0.1, 0], [2.8, 0.2, L + 0.4], "#4a423a", rough=0.5),
        box("LVL_Ceiling", [0, 3.1, 0], [2.8, 0.2, L + 0.4], "#38352f"),
        box("LVL_Wall_L", [-1.3, 1.5, 0], [0.2, 3.0, L], "#4e4a42"),
        box("LVL_Wall_R", [1.3, 1.5, 0], [0.2, 3.0, L], "#4e4a42"),
        box("LVL_Wall_End", [0, 1.5, L / 2 + 0.1], [2.8, 3.0, 0.2], "#4e4a42"),
        box("LVL_Wall_Start", [0, 1.5, -L / 2 - 0.1], [2.8, 3.0, 0.2], "#4e4a42"),
        box("ENV_Sign", [0, 1.9, L / 2 - 0.05], [1.6, 0.5, 0.1], "#203028", emissive=[0.2, 1.0, 0.5], emissive_intensity=3.0, group="ENV"),
        point("LGT_Bulb_1", [0, 2.8, -5.0], 6.5, 7.0, [1.0, 0.15, 0.08]),
        point("LGT_Bulb_2", [0, 2.8, 0.0], 6.5, 7.0, [1.0, 0.15, 0.08]),
        point("LGT_Bulb_3", [0, 2.8, 5.0], 6.5, 7.0, [1.0, 0.15, 0.08]),
        sun_entity(),
    ]
    return {
        "id": "gr4",
        "title": "GR-4 暗い廊下 + 自己発光の看板 + 赤い電球 3 灯",
        "description": "幅 2.4m・長さ 20m の密閉した廊下。突き当たりの緑の発光看板と、天井の赤い電球 3 灯。ホラーの典型(太陽は強度 0)。",
        "spec": {"version": 1, "name": "gr4_corridor", "verify": False, "entities": ents},
        "sun": {"azimuth": 270, "elevation": 33, "intensity": 0, "ambient": 0, "kelvin": 5600},
        "ddgi": {"min": [-1.2, 0.0, -10.0], "max": [1.2, 3.0, 10.0], "spacing": 0.8},
        "cameras": [{"name": "main", "position": [0.0, 1.5, -9.4], "target": [0.0, 1.5, 9.0], "displayEv": 2.0}],
        "regions": {
            "floor": {"entities": ["LVL_Floor"]},
            "ceiling": {"entities": ["LVL_Ceiling"]},
            "walls": {"entities": ["LVL_Wall_L", "LVL_Wall_R"]},
            "wall_end": {"entities": ["LVL_Wall_End"]},
            "sign": {"entities": ["ENV_Sign"]},
            "near_sign": {"entities": ["LVL_Wall_End", "LVL_Floor", "LVL_Ceiling", "LVL_Wall_L", "LVL_Wall_R"], "nearEntity": "ENV_Sign", "maxDist": 1.5},
        },
        "criteria": {"information": {"regions": ["floor", "ceiling", "walls", "wall_end", "near_sign"], "cameras": ["main"],
                                     "note": "ホラー廊下(情報のみ)。発光看板まわり near_sign は S0b/S3 で効く"}},
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).resolve().parents[1] / "scenes" / "gi"))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    names = {"gr1": "gr1_small_window", "gr2": "gr2_closed_room", "gr3": "gr3_partition", "gr4": "gr4_corridor"}
    for fn in (gr1, gr2, gr3, gr4):
        d = fn()
        p = out / f"{names[d['id']]}.json"
        p.write_text(json.dumps(d, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(p)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
