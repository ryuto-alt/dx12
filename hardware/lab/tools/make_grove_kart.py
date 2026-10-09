"""GroveLab のカートレース（scenes/kart.json と kart/track.txt）を作る。

    python hardware/lab/tools/make_grove_kart.py

コースは制御点を Catmull-Rom でなめらかにつないだ閉じた曲線。道路・縁石は箱を並べて作る。
kart/track.txt は 2m ごとの中心線（GroveKart.lua が周回・順位・コースアウトの判定に読む）。
全体の設定（ポスト・影など）は grove.json から引き継ぐ。
"""
import json
import math
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "GroveLab", "assets")
WIDTH = 12.0            # 道幅
random.seed(7)

# 制御点 (x, z)。スタートは (0, -30) 付近から +Z へ
CTRL = [(0, -60), (0, 20), (6, 70), (30, 100), (62, 104), (84, 82), (84, 52), (62, 30),
        (58, 4), (78, -20), (96, -48), (90, -84), (60, -104), (24, -100)]


def catmull(p0, p1, p2, p3, t):
    t2, t3 = t * t, t * t * t
    return tuple(0.5 * ((2 * p1[i]) + (-p0[i] + p2[i]) * t + (2 * p0[i] - 5 * p1[i] + 4 * p2[i] - p3[i]) * t2
                        + (-p0[i] + 3 * p1[i] - 3 * p2[i] + p3[i]) * t3) for i in range(2))


def dense_curve(step):
    raw = []
    n = len(CTRL)
    for i in range(n):
        p0, p1, p2, p3 = CTRL[i - 1], CTRL[i], CTRL[(i + 1) % n], CTRL[(i + 2) % n]
        for k in range(200):
            raw.append(catmull(p0, p1, p2, p3, k / 200))
    # 等間隔に取り直す
    out = [raw[0]]
    acc = 0.0
    for a, b in zip(raw, raw[1:] + raw[:1]):
        d = math.dist(a, b)
        while acc + d >= step:
            t = (step - acc) / d
            a = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
            d = math.dist(a, b)
            out.append(a)
            acc = 0.0
        acc += d
    if math.dist(out[-1], out[0]) < step * 0.5:
        out.pop()
    return out


_guid = [0]


def guid():
    _guid[0] += 1
    return f"6b617274{_guid[0]:08x}"   # "kart" + 連番


def ent(name, parent=None, **kw):
    e = {"guid": guid(), "name": name}
    if parent:
        e["parentGuid"] = parent["guid"]
    tr = {"position": [round(v, 3) for v in kw.pop("pos", (0, 0, 0))]}
    if "rot" in kw:
        tr["rotation"] = [round(v, 3) for v in kw.pop("rot")]
    if "scale" in kw:
        tr["scale"] = [round(v, 3) for v in kw.pop("scale")]
    e["transform"] = tr
    if "prim" in kw:
        e["primitive"] = kw.pop("prim")
    if "color" in kw:
        e["color"] = [round(v, 4) for v in kw.pop("color")]
        e["material"] = {"metallic": kw.pop("metal", 0.0), "roughness": kw.pop("rough", 0.7)}
        if "emit" in kw:
            e["material"]["emissiveColor"] = list(kw["emit"])
            e["material"]["emissiveIntensity"] = kw.pop("emitI", 1.0)
            kw.pop("emit")
    e.update(kw)
    return e


def main():
    os.makedirs(os.path.join(ASSETS, "kart"), exist_ok=True)
    base = json.load(open(os.path.join(ASSETS, "scenes", "grove.json"), encoding="utf-8"))
    scene = {k: v for k, v in base.items() if k != "entities"}
    scene["skybox"] = {"drawSkybox": True, "envMapPath": "__procedural_sky__", "iblIntensity": 0.9, "skyboxIntensity": 1.0}
    scene["ssgi"]["enabled"] = False
    scene["raytracing"]["ddgi"]["enabled"] = False
    scene["postProcess"]["vignetteOn"] = True
    scene["postProcess"]["vignette"] = 0.25
    es = []

    es.append(ent("DirectionalLight", pos=(0, 30, 0), rot=(-50, -35, 0),
                  directionalLight={"ambient": 0.55, "color": [1.0, 0.96, 0.9], "direction": [-0.45, -0.77, -0.45], "intensity": 3.0}))
    grp_env = ent("ENV"); grp_track = ent("TRACK"); grp_karts = ent("KARTS"); grp_items = ent("ITEMS")
    es += [grp_env, grp_track, grp_karts, grp_items]

    # 地面（芝生）
    es.append(ent("ENV_Grass", grp_env, prim="box", pos=(40, -0.15, 0), scale=(420, 0.3, 420), color=(0.16, 0.42, 0.14), rough=0.95))

    # コースの中心線（判定用、2m ごと）
    fine = dense_curve(2.0)
    fine = fine[20:] + fine[:20]       # スタートを 40m 先の直線の途中にする（グリッドがカーブの出口に掛からない）
    with open(os.path.join(ASSETS, "kart", "track.txt"), "w", encoding="utf-8") as f:
        f.write(f"# GroveKart のコース中心線（2m ごと、x z）。make_grove_kart.py が書く\nwidth {WIDTH}\n")
        for x, z in fine:
            f.write(f"{x:.2f} {z:.2f}\n")

    # 道路と縁石（6m ごとに 8m の板を置いて、カーブの外側のすき間を埋める）
    road = dense_curve(6.0)
    n = len(road)
    for i in range(n):
        a, b = road[i], road[(i + 1) % n]
        mx, mz = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
        yaw = math.degrees(math.atan2(b[0] - a[0], b[1] - a[1]))
        y = 0.02 if i % 2 == 0 else 0.03          # 重なりのちらつき防止
        es.append(ent(f"TRK_Road{i:03d}", grp_track, prim="box", pos=(mx, y, mz), rot=(0, yaw, 0), scale=(WIDTH, 0.06, 8.2),
                      color=(0.17, 0.17, 0.19), rough=0.85))
        nx, nz = math.cos(math.radians(yaw)), -math.sin(math.radians(yaw))   # 右向きの法線
        red = i % 2 == 0
        for side in (-1, 1):
            off = side * (WIDTH / 2 + 0.5)
            es.append(ent(f"TRK_Curb{i:03d}{'L' if side < 0 else 'R'}", grp_track, prim="box",
                          pos=(mx + nx * off, 0.07, mz + nz * off), rot=(0, yaw, 0), scale=(1.0, 0.14, 6.2),
                          color=(0.85, 0.1, 0.08) if red else (0.92, 0.92, 0.92), rough=0.6))

    # スタートライン（市松）とゲート
    s0, s1 = fine[0], fine[1]
    yaw0 = math.degrees(math.atan2(s1[0] - s0[0], s1[1] - s0[1]))
    nx, nz = math.cos(math.radians(yaw0)), -math.sin(math.radians(yaw0))
    fx, fz = math.sin(math.radians(yaw0)), math.cos(math.radians(yaw0))
    for r in range(2):
        for c in range(12):
            off = -WIDTH / 2 + 0.5 + c
            es.append(ent(f"TRK_Check{r}{c:02d}", grp_track, prim="box",
                          pos=(s0[0] + nx * off + fx * (r - 0.5), 0.05, s0[1] + nz * off + fz * (r - 0.5)),
                          rot=(0, yaw0, 0), scale=(1, 0.04, 1), color=(0.95, 0.95, 0.95) if (r + c) % 2 else (0.05, 0.05, 0.05)))
    for side in (-1, 1):
        off = side * (WIDTH / 2 + 1.5)
        es.append(ent(f"ENV_GatePole{'L' if side < 0 else 'R'}", grp_env, prim="box", pos=(s0[0] + nx * off, 3.5, s0[1] + nz * off),
                      rot=(0, yaw0, 0), scale=(0.8, 7, 0.8), color=(0.25, 0.25, 0.3), metal=0.6, rough=0.4))
    es.append(ent("ENV_GateBar", grp_env, prim="box", pos=(s0[0], 7.2, s0[1]), rot=(0, yaw0, 0), scale=(WIDTH + 4, 1.4, 0.8),
                  color=(0.12, 0.12, 0.14), metal=0.5, rough=0.4))
    for k in range(3):
        off = (k - 1) * 2.2
        es.append(ent(f"KART_Signal{k + 1}", grp_env, prim="sphere", pos=(s0[0] + nx * off - fx * 0.5, 7.2, s0[1] + nz * off - fz * 0.5),
                      scale=(1.0, 1.0, 1.0), color=(0.15, 0.03, 0.03), rough=0.3))

    # まわりの木（コースから 10m 以上離す）
    trees = 0
    while trees < 46:
        x, z = random.uniform(-60, 140), random.uniform(-150, 150)
        if min(math.dist((x, z), p) for p in fine) < WIDTH / 2 + 10:
            continue
        h = random.uniform(3, 6)
        es.append(ent(f"ENV_TreeTrunk{trees:02d}", grp_env, prim="box", pos=(x, h / 2, z), scale=(0.7, h, 0.7), color=(0.35, 0.22, 0.12)))
        r = random.uniform(2.5, 4.2)
        es.append(ent(f"ENV_TreeLeaf{trees:02d}", grp_env, prim="sphere", pos=(x, h + r * 0.6, z), scale=(r * 2, r * 1.8, r * 2),
                      color=(0.12 + random.uniform(0, 0.08), 0.38 + random.uniform(0, 0.12), 0.12), rough=0.9))
        trees += 1

    # アイテムボックス（3 か所 × 4 個）
    L = len(fine)
    for row, frac in enumerate((0.22, 0.52, 0.8)):
        i = int(L * frac)
        p, q = fine[i], fine[(i + 1) % L]
        yaw = math.atan2(q[0] - p[0], q[1] - p[1])
        nx, nz = math.cos(yaw), -math.sin(yaw)
        for c in range(4):
            off = (c - 1.5) * 2.6
            es.append(ent(f"KART_Item{row}{c}", grp_items, prim="box", pos=(p[0] + nx * off, 1.2, p[1] + nz * off), scale=(1.3, 1.3, 1.3),
                          color=(0.9, 0.75, 0.2), emit=(1.0, 0.8, 0.3), emitI=0.6, rough=0.2, metal=0.3))

    # カート（親 = 動かす単位、子 = 見た目）
    def kart(name, color):
        root = ent(name, grp_karts, pos=(0, 0, 0))
        es.append(root)
        es.append(ent(name + "_Body", root, prim="box", pos=(0, 0.55, 0), scale=(1.6, 0.5, 2.6), color=color, metal=0.3, rough=0.35))
        es.append(ent(name + "_Nose", root, prim="box", pos=(0, 0.45, 1.5), scale=(1.2, 0.3, 0.6), color=color, metal=0.3, rough=0.35))
        es.append(ent(name + "_Seat", root, prim="box", pos=(0, 0.95, -0.5), scale=(0.9, 0.5, 0.6), color=(0.1, 0.1, 0.12)))
        es.append(ent(name + "_Head", root, prim="sphere", pos=(0, 1.55, -0.35), scale=(0.75, 0.75, 0.75), color=(0.95, 0.95, 0.95), rough=0.3))
        for wx in (-0.95, 0.95):
            for wz in (-0.9, 0.95):
                es.append(ent(f"{name}_Wheel{'L' if wx < 0 else 'R'}{'B' if wz < 0 else 'F'}", root, prim="box",
                              pos=(wx, 0.35, wz), scale=(0.4, 0.7, 0.7), color=(0.06, 0.06, 0.06), rough=0.9))
    kart("KART_Player", (0.9, 0.12, 0.1))
    kart("KART_Cpu1", (0.15, 0.35, 0.95))
    kart("KART_Cpu2", (0.15, 0.75, 0.25))
    kart("KART_Cpu3", (0.95, 0.8, 0.1))

    es.append(ent("KART_Cam", None, pos=(0, 4, -38), camera={"fovDegrees": 62.0, "isActive": True}))
    es.append(ent("KART_Game", None, pos=(0, 0, 0), luaScript={"enabled": True, "scriptPath": "components/GroveKart.lua"}))

    scene["entities"] = es
    path = os.path.join(ASSETS, "scenes", "kart.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=None)
    total = sum(math.dist(fine[i], fine[(i + 1) % L]) for i in range(L))
    print(f"kart.json: {len(es)} エンティティ / コース 1 周 {total:.0f} m / 中心線 {L} 点")


if __name__ == "__main__":
    main()
