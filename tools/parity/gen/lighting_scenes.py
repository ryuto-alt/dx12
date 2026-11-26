"""ライティング評価(LS-00b)の検証シーン 6 本(LS-A〜F)を tools/parity/scenes/lighting/LS-?.json へ書き出す。

  python lighting_scenes.py [--out <dir>]      # 既定: tools/parity/scenes/lighting

形は GI 評価のシーン(gen/gi_scenes.py)と同じ + parity/lighting_scene.py の説明にある欄(models / variants / shadowRegions / texelBands / temporal / conditions)。
自作の箱・球・光・マネキン(assets/engine/characters/mannequin/mannequin.glb。自作 CC0)だけ。外部の素材は使わない。
カメラの垂直 FOV は 45°(エディタカメラ固定)。太陽の方位は +Z = 0°・+X = 90°(光が来る側)。

  LS-A 塔・柱・マネキンの影      … 影の画素の捕捉(coverage)・アクネ・縁の幅・距離帯ごとのテクセル密度。High(RT OFF)と Epic(RT ON)の 2 条件
  LS-B 接地・凹部                … 球・箱・マネキンの足元の接地、壁の凹部(コンタクトシャドウ / マイクロシャドウ / AO の評価)
  LS-C 暗い回廊                  … 点光源だけの暗部(DDGI の漏れ・黒潰れ)
  LS-D 面光源の候補              … 発光パネル + 光沢 / 粗い球(LTC 面光源の評価。RectLight の実装前はラスタに光が無い = ベースライン)
  LS-E 窓から光が差す霧の部屋    … 体積光(FogVolume の評価。PT は霧を持たないので EV は情報のみ)
  LS-F 静止 8 フレームと夕方     … 時間的な安定(1/255 以下)と、夕方(高度 10°)の cascade0 の影の抜け(AUD-01)
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gi_scenes import FLOOR_BROWN, GREEN, GREY, RED, box, point, sphere, window_wall_west  # noqa: E402

GROUND = "#9a9a94"
MANNEQUIN = "assets/engine/characters/mannequin/mannequin.glb"
MANNEQUIN_PROXY = {"size": [0.7, 1.8, 0.4]}


def sun(az, el, intensity=3.0, kelvin=5600):
    return {"azimuth": az, "elevation": el, "intensity": intensity, "ambient": 0, "kelvin": kelvin}


def sun_entity(az=0.0):
    # ランナーはシーン JSON に太陽を書かない(set_sun で与える)。エンジンの既定の太陽が無いと set_sun の対象が無いので 1 灯だけ置く。
    return {"name": "LGT_Sun", "kind": "light", "light": "directional", "group": "LGT", "at": [0, 20, 0], "rotation": [-33, az, 0],
            "components": {"directionalLight": {"intensity": 3.0, "ambient": 0.0}}}


TILE = 40.0


def ground(size_x=120.0, size_z=200.0, z=60.0):
    """広い地面。★1 枚の大きな箱(40m 超)にすると、RT 影 ON のときにラスタの地面が丸ごと消える(エンジンの不具合として報告済み。
    2026-11 の本流のビルドで確認)ので、40m 角のタイルに分けて並べる。名前は LVL_Ground_<i>_<j>(ground_names で一覧)。"""
    nx, nz = int(round(size_x / TILE)), int(round(size_z / TILE))
    out = []
    for i in range(nx):
        for j in range(nz):
            x = (i - (nx - 1) / 2.0) * TILE
            zc = z + (j - (nz - 1) / 2.0) * TILE
            out.append(box(f"LVL_Ground_{i}_{j}", [x, -0.1, zc], [TILE, 0.2, TILE], GROUND, rough=0.9))
    return out


def ground_names(size_x=120.0, size_z=200.0):
    return [f"LVL_Ground_{i}_{j}" for i in range(int(round(size_x / TILE))) for j in range(int(round(size_z / TILE)))]


def model(name, at):
    return {"name": name, "path": MANNEQUIN, "at": list(at), "proxy": dict(MANNEQUIN_PROXY)}


def scene(id_, title, desc, ents, sunv, cameras, regions=None, **extra):
    d = {"id": id_, "title": title, "description": desc,
         "spec": {"version": 1, "name": id_.lower().replace("-", "_"), "verify": False, "entities": ents},
         "sun": sunv, "ddgi": extra.pop("ddgi", {"min": [-10.0, 0.0, -10.0], "max": [10.0, 4.0, 10.0], "spacing": 1.0, "follow": True}),
         "cameras": cameras, "regions": regions or {}, "criteria": extra.pop("criteria", {})}
    d.update(extra)
    return d


def ls_a() -> dict:
    GN = ground_names()
    ents = [*ground(),
            box("ENV_Tower", [-6, 5, 12], [2.4, 10, 2.4], "#c8c4bc", rough=0.8)]
    bands = [(6, 4.5), (15, -4.5), (30, 4.5), (60, -4.5), (120, 4.5)]
    for z, x in bands:
        ents.append(box(f"ENV_Pillar_{z}", [x, 2, z], [1.0, 4.0, 1.0], "#c8c4bc", rough=0.8))
    ents.append(sun_entity(140))
    sr = {"shadow_tower": {"receivers": GN, "occluders": ["ENV_Tower"], "state": "roi", "roiPx": 40},
          "shadow_char": {"receivers": GN, "occluders": ["Mannequin"], "state": "roi", "roiPx": 30},
          "lit_ground": {"receivers": GN, "occluders": ["ENV_Tower", "Mannequin"] + [f"ENV_Pillar_{z}" for z, _ in bands],
                         "state": "lit", "roiPx": 60}}
    for z, _ in bands:
        sr[f"shadow_pillar_{z}"] = {"receivers": GN, "occluders": [f"ENV_Pillar_{z}"], "state": "roi", "roiPx": 30}
    return scene(
        "LS-A", "LS-A 塔・柱・マネキンの影",
        "広い地面に高さ 10m の塔(視点から 12m)、距離 6/15/30/60/120m の柱、4m 先の自作マネキン(スキンド)。太陽 方位 140°/高度 35°。"
        "影の画素の捕捉・アクネ・縁の幅・距離帯ごとのテクセル密度を見る。High(RT OFF)と Epic(RT ON)の 2 条件で撮る。",
        ents, sun(140, 35),
        [{"name": "main", "position": [0, 2.2, -6], "target": [0, 1.0, 30],
          "shadowRegions": ["shadow_tower", "lit_ground"] + [f"shadow_pillar_{z}" for z, _ in bands]},
         {"name": "char", "position": [-1.6, 1.5, 0.8], "target": [1.4, 0.4, 5.5], "shadowRegions": ["shadow_char"],
          "note": "マネキンの影の縁(スキンドの影)を大きく撮る補助カメラ",
          "contaminated": "このカメラのラスタは塔の上部が欠けて見える(PT には出る)。原因が分かるまで、このカメラの値はベースラインに使わない(エンジン不具合の疑い。C レーンへ依頼済み)"}],
        models=[model("Mannequin", [1.4, 0, 5.0])],
        shadowRegions=sr,
        texelBands=[5, 10, 20, 40, 80, 160],
        conditions=["high", "epic"],
        criteria={"coverage": {"region": "shadow_tower", "camera": "main", "min_ratio": 0.95},
                  "acne": {"region": "lit_ground", "camera": "main", "max_rate": 0.001},
                  "edge_char": {"region": "shadow_char", "camera": "char"}})


def ls_b() -> dict:
    ents = [box("LVL_Floor", [0, -0.1, 0], [10.4, 0.2, 10.4], FLOOR_BROWN, rough=0.6),
            box("LVL_Wall_N", [0, 1.6, 5.1], [10.4, 3.2, 0.2], GREY),
            box("LVL_Wall_W", [-5.1, 1.6, 0], [0.2, 3.2, 10.4], GREY),
            # 壁の凹部(アルコーブ): 北壁の手前に幅 1.2m・奥行き 0.6m の凹みを作る(両脇の出っ張りと上下の張り出し)
            box("ENV_RecessL", [-1.0, 1.0, 4.7], [0.6, 2.0, 0.6], GREY),
            box("ENV_RecessR", [1.0, 1.0, 4.7], [0.6, 2.0, 0.6], GREY),
            box("ENV_RecessTop", [0, 2.1, 4.7], [2.2, 0.2, 0.6], GREY),
            sphere("ENV_Sphere", [-2.5, 0.5, 1.5], 1.0, "#d8d4cc", rough=0.7),
            box("ENV_Box", [2.5, 0.4, 1.5], [0.8, 0.8, 0.8], "#d8d4cc", rough=0.7),
            sun_entity(100)]
    sr = {"shadow_box": {"receivers": ["LVL_Floor"], "occluders": ["ENV_Box"], "state": "roi", "roiPx": 30},
          "shadow_char": {"receivers": ["LVL_Floor"], "occluders": ["Mannequin"], "state": "roi", "roiPx": 30}}
    return scene(
        "LS-B", "LS-B 接地・凹部",
        "10x3.2x10m の部屋。床に置いた球・箱・マネキンの足元の接地と、北壁の凹部(幅 1.2m)の隅の暗さを見る。太陽 方位 100°/高度 40°。",
        ents, sun(100, 40),
        [{"name": "main", "position": [0.5, 1.8, -4.2], "target": [0, 0.8, 2.5], "shadowRegions": ["shadow_box", "shadow_char"]},
         {"name": "recess", "position": [0, 1.5, 0.0], "target": [0, 1.2, 4.7], "shadowRegions": []}],
        regions={"floor": {"entities": ["LVL_Floor"]},
                 "floor_near_sphere": {"entities": ["LVL_Floor"], "nearEntity": "ENV_Sphere", "maxDist": 0.45},
                 "floor_near_box": {"entities": ["LVL_Floor"], "nearEntity": "ENV_Box", "maxDist": 0.45},
                 "recess": {"entities": ["ENV_RecessL", "ENV_RecessR", "LVL_Wall_N"], "nearEntity": "ENV_RecessTop", "maxDist": 1.0}},
        models=[model("Mannequin", [0.0, 0, 2.6])],
        shadowRegions=sr,
        conditions=["high", "epic"],
        criteria={"information": {"regions": ["floor_near_sphere", "floor_near_box", "recess"], "cameras": ["main", "recess"]}})


def ls_c() -> dict:
    ents = [box("LVL_Floor", [0, -0.1, 0], [2.8, 0.2, 20.4], "#4a423a", rough=0.5),
            box("LVL_Ceiling", [0, 3.1, 0], [2.8, 0.2, 20.4], "#38352f", rough=0.9),
            box("LVL_Wall_L", [-1.3, 1.5, 0], [0.2, 3.0, 20.0], "#4e4a42", rough=0.9),
            box("LVL_Wall_R", [1.3, 1.5, 0], [0.2, 3.0, 20.0], "#4e4a42", rough=0.9),
            box("LVL_Wall_End", [0, 1.5, 10.1], [2.8, 3.2, 0.2], "#4e4a42", rough=0.9),
            box("LVL_Wall_Start", [0, 1.5, -10.1], [2.8, 3.2, 0.2], "#4e4a42", rough=0.9),
            box("ENV_Crate", [0.6, 0.4, 3.0], [0.8, 0.8, 0.8], "#6a5a46", rough=0.8),
            point("LGT_Bulb_1", [0, 2.8, -4.0], 6.0, 7.0, [1.0, 0.25, 0.15]),
            point("LGT_Bulb_2", [0, 2.8, 2.0], 6.0, 7.0, [1.0, 0.25, 0.15]),
            point("LGT_Bulb_3", [0, 2.8, 8.0], 6.0, 7.0, [1.0, 0.25, 0.15]),
            sun_entity(270)]
    return scene(
        "LS-C", "LS-C 暗い回廊",
        "幅 2.4m・長さ 20m の密閉した廊下。天井に赤い電球 3 灯、床に木箱。太陽は 0。暗部の黒潰れと DDGI の漏れを見る(GR-4 と同系)。",
        ents, sun(270, 33, intensity=0),
        [{"name": "main", "position": [0, 1.6, -9.0], "target": [0, 1.3, 9.0]},
         {"name": "side", "position": [-0.9, 1.4, 0.0], "target": [0.9, 0.9, 7.0]}],
        regions={"walls": {"entities": ["LVL_Wall_L", "LVL_Wall_R"]}, "floor": {"entities": ["LVL_Floor"]},
                 "ceiling": {"entities": ["LVL_Ceiling"]}, "end": {"entities": ["LVL_Wall_End"]},
                 "crate": {"entities": ["ENV_Crate"]}},
        ddgi={"min": [-1.4, 0.0, -10.0], "max": [1.4, 3.1, 10.0], "spacing": 0.8},
        conditions=["epic"],
        criteria={"information": {"regions": ["walls", "floor", "ceiling", "end", "crate"], "cameras": ["main", "side"]}})


def ls_d() -> dict:
    ents = [box("LVL_Floor", [0, -0.1, 0], [8.4, 0.2, 6.4], "#8a8680", rough=0.35),
            box("LVL_Wall_N", [0, 1.6, 3.1], [8.4, 3.2, 0.2], GREY),
            box("LVL_Wall_W", [-4.1, 1.6, 0], [0.2, 3.2, 6.4], GREY),
            box("LVL_Wall_E", [4.1, 1.6, 0], [0.2, 3.2, 6.4], GREY),
            box("LVL_Ceiling", [0, 3.3, 0], [8.4, 0.2, 6.4], GREY),
            box("LVL_Wall_S", [0, 1.6, -3.1], [8.4, 3.2, 0.2], GREY),
            box("ENV_Panel", [0, 2.0, 2.95], [2.4, 1.2, 0.08], "#ffffff", rough=0.5,
                emissive=[1.0, 0.95, 0.85], emissive_intensity=8.0),
            sphere("ENV_SphereGloss", [-1.2, 0.5, 1.2], 1.0, "#e0e0e0", rough=0.08),
            sphere("ENV_SphereRough", [1.2, 0.5, 1.2], 1.0, "#e0e0e0", rough=0.6),
            sun_entity(270)]
    return scene(
        "LS-D", "LS-D 面光源の候補",
        "奥の壁に 2.4x1.2m の発光パネル(8 倍)。手前に光沢球と粗い球。太陽は 0。面光源の形に沿ったハイライトと壁・床への回り込みを見る。"
        "RectLight の実装前は、ラスタにはパネルから出る光が無い(発光は自分の画素だけ)= ベースライン。",
        ents, sun(270, 33, intensity=0),
        [{"name": "main", "position": [0, 1.5, -2.6], "target": [0, 1.1, 3.0]}],
        regions={"panel": {"entities": ["ENV_Panel"]}, "floor_near_panel": {"entities": ["LVL_Floor"], "nearEntity": "ENV_Panel", "maxDist": 3.0},
                 "wall_near_panel": {"entities": ["LVL_Wall_N"], "nearEntity": "ENV_Panel", "maxDist": 1.5},
                 "sphere_gloss": {"entities": ["ENV_SphereGloss"]}, "sphere_rough": {"entities": ["ENV_SphereRough"]}},
        ddgi={"min": [-4.0, 0.0, -3.0], "max": [4.0, 3.2, 3.0], "spacing": 0.8},
        conditions=["epic"],
        criteria={"information": {"regions": ["floor_near_panel", "wall_near_panel", "sphere_gloss", "sphere_rough"], "cameras": ["main"]}})


def ls_e() -> dict:
    ents = [box("LVL_Floor", [0, -0.1, 0], [8.4, 0.2, 6.4], FLOOR_BROWN, rough=0.6),
            box("LVL_Ceiling", [0, 3.3, 0], [8.4, 0.2, 6.4], GREY),
            box("LVL_Wall_N", [0, 1.6, 3.1], [8.4, 3.2, 0.2], GREY),
            box("LVL_Wall_S", [0, 1.6, -3.1], [8.4, 3.2, 0.2], GREY),
            box("LVL_Wall_E", [4.1, 1.6, 0], [0.2, 3.2, 6.4], GREEN),
            *window_wall_west("LVL_Wall_W", -4.1, 3.2, 3.2, [-1.0, 1.0], [1.0, 2.4]),
            box("LVL_Ground", [0, -0.45, 0], [40, 0.2, 40], "#6e6a5e", rough=0.95),
            box("ENV_Rug", [-1.7, 0.015, 0], [2.2, 0.03, 2.2], RED),
            sun_entity(270)]
    return scene(
        "LS-E", "LS-E 窓から光が差す霧の部屋",
        "GR-1 と同じ小窓の部屋(西壁の窓から太陽 方位 270°/高度 33°)。体積霧(fog 条件)を足して光芒を見る。パストレーサーは霧を持たないので EV は情報のみ。",
        ents, sun(270, 33),
        [{"name": "main", "position": [3.3, 1.7, -2.7], "target": [-2, 0.9, 1]}],
        regions={"walls": {"entities": ["LVL_Wall_N", "LVL_Wall_S", "LVL_Wall_E"]}, "floor": {"entities": ["LVL_Floor"]},
                 "rug": {"entities": ["ENV_Rug"]}},
        ddgi={"min": [-4.0, 0.0, -3.0], "max": [4.0, 3.2, 3.0], "spacing": 0.8},
        conditions=["epic", "epic_fog"],
        criteria={"information": {"regions": ["walls", "floor", "rug"], "cameras": ["main"]}})


def ls_f() -> dict:
    GN = ground_names(80, 120)
    ents = [*ground(80, 120, 30),
            box("ENV_Tower", [3.0, 3.0, 6.0], [1.6, 6.0, 1.6], "#c8c4bc", rough=0.8),
            box("ENV_Pillar_10", [-3.0, 1.5, 10.0], [0.8, 3.0, 0.8], "#c8c4bc", rough=0.8),
            sphere("ENV_Sphere", [-1.0, 0.6, 3.0], 1.2, "#d0a070", rough=0.4),
            # カメラの後ろ(太陽側)の高い塔。夕方(方位 200°/高度 10°)では、その影がカメラの前の cascade0 の範囲へ長く伸びる(AUD-01:
            # 光側のカメラ後方の遮蔽物が、cascade0 の光の錐台の外にあって落ちる / 近平面で切られる不具合の検出用)。
            box("ENV_BehindCaster", [-8.0, 6.0, -13.0], [2.0, 12.0, 2.0], "#c8c4bc", rough=0.8),
            sun_entity(250)]
    sr = {"shadow_tower": {"receivers": GN, "occluders": ["ENV_Tower"], "state": "roi", "roiPx": 40},
          "shadow_behind": {"receivers": GN, "occluders": ["ENV_BehindCaster"], "state": "roi", "roiPx": 30, "minDepth": 3.0, "maxDepth": 40.0},
          "shadow_pillar": {"receivers": GN, "occluders": ["ENV_Pillar_10"], "state": "roi", "roiPx": 30},
          "lit_ground": {"receivers": GN, "occluders": ["ENV_Tower", "ENV_Pillar_10", "ENV_Sphere", "ENV_BehindCaster"], "state": "lit", "roiPx": 60}}
    return scene(
        "LS-F", "LS-F 静止 8 フレームと夕方",
        "地面に塔(6m)・柱・球。カメラも物も静止で、連続 8 フレームの 8bit 表示の最大差(1/255 以下が目標)を見る。"
        "夕方(太陽 方位 200°/高度 10°。カメラの後ろから前へ長く伸びる)の条件では、カメラの後ろにある高い塔(ENV_BehindCaster)の影が"
        "カメラの前の cascade0 の範囲へ伸びて落ちないことを見る(AUD-01。遮蔽物が光の錐台の外・カメラの後ろにある場合)。",
        ents, sun(250, 50),
        [{"name": "main", "position": [0, 2.0, -5.0], "target": [0, 1.0, 14.0],
          "shadowRegions": ["shadow_tower", "shadow_pillar", "shadow_behind", "lit_ground"]}],
        variants=[{"id": "noon", "sun": sun(250, 50)}, {"id": "evening10", "sun": sun(200, 10, intensity=2.2, kelvin=3800)}],
        shadowRegions=sr,
        temporal={"frames": 8, "settle": 120},
        conditions=["high", "epic"],
        criteria={"temporal": {"max_range": 1},
                  "coverage": {"region": "shadow_tower", "camera": "main", "min_ratio": 0.95, "variant": "noon"},
                  "coverage_behind": {"region": "shadow_behind", "camera": "main", "min_ratio": 0.95, "variant": "evening10"},
                  "acne": {"region": "lit_ground", "camera": "main", "max_rate": 0.001}})


SCENES = {"LS-A": ls_a, "LS-B": ls_b, "LS-C": ls_c, "LS-D": ls_d, "LS-E": ls_e, "LS-F": ls_f}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).resolve().parents[1] / "scenes" / "lighting"))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    for sid, fn in SCENES.items():
        (out / f"{sid}.json").write_text(json.dumps(fn(), indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print("書いた:", out / f"{sid}.json")


if __name__ == "__main__":
    main()
