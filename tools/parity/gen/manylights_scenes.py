"""SPK-1(ReSTIR DI / SHaRC の計測スパイク)用の多灯シーン ml_64 / ml_256 を tools/parity/scenes/manylights/*.json へ書き出す。

  python manylights_scenes.py [--out <dir>]      # 既定: tools/parity/scenes/manylights

形は GI / ライティング評価のシーン(gen/gi_scenes.py・gen/lighting_scenes.py)と同じ。parity.gi_scene.load_scene でそのまま読める。
自作の箱・球・点光源だけ(外部の素材なし)。灯の位置・色は固定のシードから決める(再生成しても同じファイルになる)。

構成(両シーン共通の形状。灯の数だけが違う):
  中庭   24x24m・高さ 6m の屋根付きホール(空の寄与を消すため天井あり)。柱 8 本・植木鉢・中央の台・低い仕切り壁 3 枚(灯が壁越しに漏れるかを見る)。
  回廊   中庭の南壁(z=-12)の開口(幅 4m)から -z 方向へ 30m。幅 4m・高さ 3.2m。天井に 2 列で灯を吊る。
  灯数   ml_64 = 64 灯(回廊 16 + 中庭 48)/ ml_256 = 256 灯(回廊 64 + 中庭 192)。
         256 灯は 1 灯の強さと半径を小さくして、全体の明るさが ml_64 と同程度になるようにしてある。
  影     点光源の castShadows は全灯 false。ラスタの点光源の影の上限(16 灯)を超えるので、現行のラスタ = 影なしの総当たりが基準線。
         PT(パストレーサー)は全灯に遮蔽を見る = 正解。ReSTIR DI の試作は可視性レイで PT に近づける狙い。
太陽は 0(set_sun intensity 0)。ランナーは set_sun で与える。
"""
from __future__ import annotations

import argparse
import json
import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gi_scenes import box, point, sphere  # noqa: E402
from lighting_scenes import scene, sun, sun_entity  # noqa: E402

WALL = "#cfc9bd"
STONE = "#8d877c"
DARK = "#4a463f"
WOOD = "#6a5a46"
SEED = 20261127

# 強さ × 灯の数 ≒ 一定(ml_256 は半径も絞る)
VARIANTS = {
    "ml_64": {"n": 64, "intensity": 7.0, "range_court": 6.0, "range_corr": 5.0},
    "ml_256": {"n": 256, "intensity": 2.2, "range_court": 4.5, "range_corr": 3.5},
}

COURT_HALF = 12.0
COURT_H = 6.0
CORR_LEN = 30.0
CORR_HALF_W = 2.0
CORR_H = 3.2

WARM = [1.0, 0.62, 0.30]
AMBER = [1.0, 0.78, 0.45]
COOL = [0.45, 0.65, 1.0]
SOFT = [1.0, 0.88, 0.72]
COURT_PALETTE = [WARM, AMBER, COOL, SOFT, WARM, AMBER]
CORR_PALETTE = [WARM, WARM, AMBER, SOFT]


def geometry() -> list[dict]:
    e: list[dict] = []
    h = COURT_HALF
    # 中庭の床・天井(12m 角のタイル 2x2。大きな箱は RT 影で消える既知の不具合を避ける)
    for i, x in enumerate((-6.0, 6.0)):
        for j, z in enumerate((-6.0, 6.0)):
            e.append(box(f"LVL_Floor_{i}{j}", [x, -0.1, z], [12.0, 0.2, 12.0], STONE, rough=0.55))
            e.append(box(f"LVL_Roof_{i}{j}", [x, COURT_H + 0.1, z], [12.0, 0.2, 12.0], DARK, rough=0.9))
    # 外壁(南壁は回廊の開口で 3 枚に分ける)
    e.append(box("LVL_Wall_N", [0, COURT_H / 2, h + 0.1], [24.4, COURT_H, 0.2], WALL))
    e.append(box("LVL_Wall_E", [h + 0.1, COURT_H / 2, 0], [0.2, COURT_H, 24.4], WALL))
    e.append(box("LVL_Wall_W", [-h - 0.1, COURT_H / 2, 0], [0.2, COURT_H, 24.4], WALL))
    seg = h - CORR_HALF_W
    e.append(box("LVL_Wall_S_L", [-(CORR_HALF_W + seg / 2), COURT_H / 2, -h - 0.1], [seg + 0.2, COURT_H, 0.2], WALL))
    e.append(box("LVL_Wall_S_R", [(CORR_HALF_W + seg / 2), COURT_H / 2, -h - 0.1], [seg + 0.2, COURT_H, 0.2], WALL))
    e.append(box("LVL_Wall_S_Top", [0, (COURT_H + CORR_H) / 2, -h - 0.1], [2 * CORR_HALF_W, COURT_H - CORR_H, 0.2], WALL))
    # 回廊(長さ 30m。40m 未満なので 1 枚物でよい)
    zc = -h - CORR_LEN / 2
    e.append(box("LVL_Corr_Floor", [0, -0.1, zc], [2 * CORR_HALF_W + 0.4, 0.2, CORR_LEN], "#6e675c", rough=0.4))
    e.append(box("LVL_Corr_Ceiling", [0, CORR_H + 0.1, zc], [2 * CORR_HALF_W + 0.4, 0.2, CORR_LEN], DARK))
    e.append(box("LVL_Corr_Wall_L", [-CORR_HALF_W - 0.1, CORR_H / 2, zc], [0.2, CORR_H, CORR_LEN], WALL))
    e.append(box("LVL_Corr_Wall_R", [CORR_HALF_W + 0.1, CORR_H / 2, zc], [0.2, CORR_H, CORR_LEN], WALL))
    e.append(box("LVL_Corr_Wall_End", [0, CORR_H / 2, -h - CORR_LEN - 0.1], [2 * CORR_HALF_W + 0.4, CORR_H, 0.2], WALL))
    # 回廊の柱の出っ張り(灯の遮蔽)
    for k, z in enumerate((-18.0, -26.0, -34.0)):
        e.append(box(f"ENV_Corr_Pier_{k}L", [-CORR_HALF_W + 0.25, CORR_H / 2, z], [0.5, CORR_H, 0.6], STONE))
        e.append(box(f"ENV_Corr_Pier_{k}R", [CORR_HALF_W - 0.25, CORR_H / 2, z], [0.5, CORR_H, 0.6], STONE))
    # 中庭の柱 8 本
    for k, (x, z) in enumerate([(-7, -7), (0, -7), (7, -7), (-7, 0), (7, 0), (-7, 7), (0, 7), (7, 7)]):
        e.append(box(f"ENV_Pillar_{k}", [x, COURT_H / 2, z], [1.0, COURT_H, 1.0], WALL, rough=0.8))
    # 中央の台・低い仕切り壁・植木鉢(遮蔽物)
    e.append(box("ENV_Dais", [0, 0.2, 0], [6.0, 0.4, 6.0], "#a09a8e", rough=0.5))
    e.append(box("ENV_LowWall_A", [-3.5, 0.6, 3.5], [0.3, 1.2, 6.0], WALL))
    e.append(box("ENV_LowWall_B", [3.5, 0.6, -3.5], [0.3, 1.2, 6.0], WALL))
    e.append(box("ENV_LowWall_C", [0, 0.6, 9.0], [8.0, 1.2, 0.3], WALL))
    for k, (x, z) in enumerate([(-10, 4), (-10, -4), (10, 4), (10, -4), (-4, 10), (4, 10)]):
        e.append(box(f"ENV_Planter_{k}", [x, 0.4, z], [2.0, 0.8, 1.0], WOOD, rough=0.8))
    e.append(sphere("ENV_Orb_A", [-1.2, 1.4, 0.0], 1.0, "#d8d8d8", rough=0.12))
    e.append(sphere("ENV_Orb_B", [1.2, 1.4, 0.5], 1.0, "#c8a060", rough=0.4, metallic=1.0))
    return e


def lights(n: int, intensity: float, rng_court: float, rng_corr: float) -> list[dict]:
    """回廊 n/4 灯(2 列・等間隔)+ 中庭 3n/4 灯(層化した格子 + ジッタ)。固定シード。"""
    rnd = random.Random(SEED)
    n_corr = n // 4
    n_court = n - n_corr
    out: list[dict] = []
    # 回廊: 天井の下 0.3m に 2 列。z は -12.8 から -41.2 へ等間隔
    per_row = n_corr // 2
    for row, x in enumerate((-0.8, 0.8)):
        for i in range(per_row):
            z = -12.8 - (i + 0.5) * (28.4 / per_row)
            col = CORR_PALETTE[(i + row) % len(CORR_PALETTE)]
            out.append(point(f"LGT_Corr_{row}_{i:02d}", [x, CORR_H - 0.3, round(z, 3)], intensity, rng_corr, col, cast=False))
    # 中庭: g x g のセルにジッタ付きで 1 灯ずつ(壁の外側 1m は避ける)
    g = math.ceil(math.sqrt(n_court))
    cells = [(i, j) for i in range(g) for j in range(g)]
    rnd.shuffle(cells)
    cells = sorted(cells[:n_court])
    span = 2 * (COURT_HALF - 1.0)
    for k, (i, j) in enumerate(cells):
        x = -COURT_HALF + 1.0 + (i + 0.15 + 0.7 * rnd.random()) * span / g
        z = -COURT_HALF + 1.0 + (j + 0.15 + 0.7 * rnd.random()) * span / g
        y = 1.0 + 3.8 * rnd.random()
        col = COURT_PALETTE[rnd.randrange(len(COURT_PALETTE))]
        out.append(point(f"LGT_Court_{k:03d}", [round(x, 3), round(y, 3), round(z, 3)], intensity, rng_court, col, cast=False))
    assert len(out) == n, (len(out), n)
    return out


def build(vid: str) -> dict:
    v = VARIANTS[vid]
    ents = [*geometry(), *lights(v["n"], v["intensity"], v["range_court"], v["range_corr"]), sun_entity(270)]
    cams = [
        {"name": "court_wide", "position": [0.0, 1.7, 11.0], "target": [0.0, 1.8, -8.0]},
        {"name": "court_corner", "position": [-10.5, 2.4, 10.5], "target": [3.0, 1.0, -4.0]},
        {"name": "threshold", "position": [0.0, 1.6, -9.0], "target": [0.0, 1.4, -30.0]},
        {"name": "corridor_deep", "position": [0.0, 1.6, -14.0], "target": [0.0, 1.4, -41.0]},
    ]
    regions = {
        "court_floor": {"entities": [f"LVL_Floor_{i}{j}" for i in (0, 1) for j in (0, 1)] + ["ENV_Dais"]},
        "court_walls": {"entities": ["LVL_Wall_N", "LVL_Wall_E", "LVL_Wall_W", "LVL_Wall_S_L", "LVL_Wall_S_R"]},
        "pillars": {"entities": [f"ENV_Pillar_{k}" for k in range(8)]},
        "low_walls": {"entities": ["ENV_LowWall_A", "ENV_LowWall_B", "ENV_LowWall_C"]},
        "corr_floor": {"entities": ["LVL_Corr_Floor"]},
        "corr_walls": {"entities": ["LVL_Corr_Wall_L", "LVL_Corr_Wall_R", "LVL_Corr_Wall_End"]},
    }
    return scene(
        vid.replace("ml_", "ML-"), f"ML-{v['n']} 多灯の中庭と回廊({v['n']} 灯)",
        f"24x24m の屋根付き中庭と、そこから伸びる 30m の回廊。点光源 {v['n']} 灯(回廊 {v['n'] // 4} + 中庭 {v['n'] - v['n'] // 4})。"
        "太陽は 0・全灯 castShadows なし。SPK-1(ReSTIR DI / SHaRC の計測)の基準シーン。PT が正解、現行ラスタ(影なし総当たり)が基準線。",
        ents, sun(270, 33, intensity=0), cams, regions=regions,
        ddgi={"min": [-12.0, 0.0, -42.0], "max": [12.0, 6.0, 12.0], "spacing": 3.0},
        conditions=["epic"],
        criteria={"information": {"regions": list(regions), "cameras": [c["name"] for c in cams]}},
        spike={"id": "SPK-1", "lights": v["n"], "lightIntensity": v["intensity"], "rangeCourt": v["range_court"],
               "rangeCorridor": v["range_corr"], "internalResolution": [1280, 720], "seed": SEED})


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).resolve().parents[1] / "scenes" / "manylights"))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    for vid in VARIANTS:
        p = out / f"{vid}.json"
        p.write_text(json.dumps(build(vid), indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print("書いた:", p)


if __name__ == "__main__":
    main()
