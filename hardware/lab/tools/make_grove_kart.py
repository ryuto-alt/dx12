"""GroveLab のカートレース（scenes/kart.json と kart/track.txt / kart/layout.json）を作る。

    python hardware/lab/tools/make_grove_kart.py

コースは制御点を Catmull-Rom でなめらかにつないだ閉じた曲線（全長およそ 850m）。
  スタート直線（観客席）→ 右の大回り → 森の S 字 → トンネルのバックストレート → ヘアピン（タイヤの壁）
  → 湖の橋 → 大回りの最終コーナー
見た目（道・地面・湖・木・小物・カート・アイテム箱）は Blender で作った glb（assets/models/kart/）を置くだけ。
  Blender 側は kart/layout.json（1m ごとの中心線と見どころの区間）を読んで同じ座標で作る。
  道や木はまとめた少数のメッシュにしてある（画面分割で 4 回描いても描画回数が増えすぎないように）。
kart/track.txt は 2m ごとの中心線（GroveKart.lua が周回・順位・コースアウトの判定に読む）。
GroveKart.lua が名前で探すもの（KART_Player / KART_Cpu1..3 / KART_Cam / KART_Signal1..3 / KART_Item<row><col>）は残す。
"""
import json
import math
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "GroveLab", "assets")
WIDTH = 12.0            # 道幅
SCALE = 1.1
random.seed(7)

# 制御点 (x, z)。スタート直線は x≈-22 を +Z へ
CTRL = [(x * SCALE, z * SCALE) for x, z in [
    (-20, -60), (-20, 20), (-12, 66), (14, 96), (52, 104), (84, 90), (90, 62), (86, 40), (100, 18), (126, 6),
    (140, -24), (138, -72), (126, -112), (100, -130), (78, -116), (80, -86), (64, -62), (38, -66),
    (24, -96), (4, -120), (-22, -116), (-30, -92)]]

KART_MODELS = {"KART_Player": "kart_red", "KART_Cpu1": "kart_blue", "KART_Cpu2": "kart_green", "KART_Cpu3": "kart_yellow"}
ITEM_FRACS = (0.235, 0.523, 0.77)   # アイテムボックスの列（1 周のうちの位置。ヘアピンや最終コーナーの途中を避ける）
COURSE_MODELS = ["course_ground", "course_road", "course_props", "course_trees_0", "course_trees_1", "course_trees_2", "course_trees_3"]


def catmull(p0, p1, p2, p3, t):
    t2, t3 = t * t, t * t * t
    return tuple(0.5 * ((2 * p1[k]) + (-p0[k] + p2[k]) * t + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t2
                        + (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t3) for k in range(2))


def dense_curve(step):
    """閉曲線を step m ごとの点列にする"""
    raw = []
    n = len(CTRL)
    for i in range(n):
        p0, p1, p2, p3 = CTRL[i - 1], CTRL[i], CTRL[(i + 1) % n], CTRL[(i + 2) % n]
        for s in range(200):
            raw.append(catmull(p0, p1, p2, p3, s / 200))
    out = [raw[0]]
    acc = 0.0
    for a, b in zip(raw, raw[1:] + raw[:1]):
        acc += math.dist(a, b)
        if acc >= step:
            out.append(b)
            acc = 0.0
    return out


def resample_exact(step, start):
    """閉曲線を、start に最も近い点から始めて弧長ちょうど step m ごとに取り直す（index = 距離 m）"""
    raw = []
    n = len(CTRL)
    for i in range(n):
        p0, p1, p2, p3 = CTRL[i - 1], CTRL[i], CTRL[(i + 1) % n], CTRL[(i + 2) % n]
        for s in range(400):
            raw.append(catmull(p0, p1, p2, p3, s / 400))
    k0 = min(range(len(raw)), key=lambda i: math.dist(raw[i], start))
    raw = raw[k0:] + raw[:k0] + [raw[k0]]
    out = [raw[0]]
    want = step
    acc = 0.0
    for a, b in zip(raw, raw[1:]):
        seg = math.dist(a, b)
        while seg > 0 and acc + seg >= want:
            t = (want - acc) / seg
            out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
            want += step
        acc += seg
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
    if "model" in kw:
        e["meshRenderer"] = {"modelPath": kw.pop("model")}
    if "color" in kw:
        e["color"] = [round(v, 4) for v in kw.pop("color")]
        e["material"] = {"metallic": kw.pop("metal", 0.0), "roughness": kw.pop("rough", 0.7)}
        if "emit" in kw:
            e["material"]["emissiveColor"] = list(kw["emit"])
            e["material"]["emissiveIntensity"] = kw.pop("emitI", 1.0)
            kw.pop("emit")
    e.update(kw)
    return e


def frame_at(pts, i):
    """i 番目の点の (位置, 進む向き, 右向き)"""
    L = len(pts)
    p, q = pts[i % L], pts[(i + 1) % L]
    yaw = math.atan2(q[0] - p[0], q[1] - p[1])
    return p, (math.sin(yaw), math.cos(yaw)), (math.cos(yaw), -math.sin(yaw)), yaw


def main():
    os.makedirs(os.path.join(ASSETS, "kart"), exist_ok=True)
    base = json.load(open(os.path.join(ASSETS, "scenes", "grove.json"), encoding="utf-8"))
    scene = {k: v for k, v in base.items() if k != "entities"}
    scene["skybox"] = {"drawSkybox": True, "envMapPath": "__procedural_sky__", "iblIntensity": 0.9, "skyboxIntensity": 1.0}
    scene["ssgi"]["enabled"] = False
    scene["raytracing"]["ddgi"]["enabled"] = False
    # 見た目: 明るいトイ調。色の調整（彩度・コントラスト・色温度・周辺減光・FXAA）は画面分割の全区画に掛かる。
    #   ブルームと SSAO は区画 1（と 1 画面のとき）だけ。TAA は画面分割で区画ごとに履歴が無いので使わず FXAA にする
    pp = scene["postProcess"]
    pp.update({"vignetteOn": True, "vignette": 0.22, "vignetteSoftness": 0.55,
               "saturationOn": True, "saturation": 1.18, "contrastOn": True, "contrast": 1.08,
               "warmthOn": True, "warmth": 0.06, "fxaaOn": True, "debandOn": True,
               "bloomOn": True, "bloom": 0.22, "bloomThreshold": 1.25, "bloomRadius": 0.6})
    scene["taa"]["enabled"] = False
    scene["ssao"].update({"enabled": True, "intensity": 0.8, "radius": 0.6})
    scene["shadowPcss"]["enabled"] = True
    # 影は CSM（+PCSS）にする。RT 影は区画 1 にしか描かれず、しかも有効にすると CSM が RT の担当分を描かなくなるので、
    #   画面分割の区画 2..N の影が消える
    scene["raytracing"]["shadowEnabled"] = False
    scene["contactShadow"]["enabled"] = False
    es = []

    # 中心線: スタートを直線の途中にする（グリッドがカーブに掛からない）
    fine = dense_curve(2.0)
    fine = fine[20:] + fine[:20]
    L = len(fine)
    with open(os.path.join(ASSETS, "kart", "track.txt"), "w", encoding="utf-8") as f:
        f.write(f"# GroveKart のコース中心線（2m ごと、x z）。make_grove_kart.py が書く\nwidth {WIDTH}\n")
        for x, z in fine:
            f.write(f"{x:.2f} {z:.2f}\n")
    total = sum(math.dist(fine[i], fine[(i + 1) % L]) for i in range(L))

    # Blender 用のレイアウト: 1m ごとの中心線（track.txt と同じ始点・向き）と、見どころの区間（中心線の距離 s [m]）
    one = resample_exact(1.0, fine[0])

    def s_near(x, z):
        i = min(range(len(one)), key=lambda j: math.dist(one[j], (x * SCALE, z * SCALE)))
        return float(i)

    def span(a, b):
        return [s_near(*a), s_near(*b)]

    items = [round(total * f, 1) for f in ITEM_FRACS]
    layout = {
        "about": "GroveLab カートのコース。座標はエンジンのワールド（x 右, y 上, z 奥）・単位 m。"
                 "centerline は 1m ごとの中心線（閉じている。index = スタートからの距離 s [m]）。zones の s は [始, 終]（始 > 終 はスタートをまたぐ）。"
                 "道は centerline の左右 width/2。start は s=0（ゴール線。+s の向きへ走る）",
        "width": WIDTH,
        "length": round(total, 1),
        "centerline": [[round(x, 3), round(z, 3)] for x, z in one],
        "start": {"s": 0.0, "gridBehind": 16.0, "gate": {"poleOffset": WIDTH / 2 + 1.5, "barHeight": 7.2, "barThick": 1.4,
                                                          "signalLights": "KART_Signal1..3 は球（エンジン側で置く）。バーの手前 0.5m・高さ 7.2・横 -2.2/0/+2.2"}},
        "zones": {
            "grandstand":  {"side": "left", "s": span((-20, -56), (-20, 16)), "note": "スタート直線の左（コースの外側）に観客席・ピットの壁"},
            "turn1":       {"s": span((-12, 66), (52, 104)), "note": "右の大回り。外側に看板・縁石を大きく"},
            "forest":      {"s": span((84, 90), (126, 6)), "note": "森の S 字。木を密に"},
            "tunnel":      {"s": span((140, -30), (138, -64)), "note": "バックストレートのトンネル（道の上をまたぐアーチ。床は y=0 のまま）"},
            "hairpin":     {"s": span((126, -112), (78, -116)), "note": "ヘアピン。外側にタイヤの壁と砂地（グラベル）"},
            "lakeBridge":  {"s": span((80, -86), (40, -66)), "note": "湖をまたぐ橋。道の高さは y=0 のまま、水面を y=-0.6 に下げて欄干を付ける"},
            "finalCorner": {"s": span((24, -96), (-30, -92)), "note": "最終コーナー。外側にタイヤの壁"},
        },
        "itemRows": items,
        "itemBox": {"size": 1.3, "note": "KART_Item<row><col> はエンジン側で itembox.glb を置く（各列 4 個、道の横方向に 2.6m 間隔）"},
    }
    with open(os.path.join(ASSETS, "kart", "layout.json"), "w", encoding="utf-8") as f:
        json.dump(layout, f, ensure_ascii=False)

    es.append(ent("DirectionalLight", pos=(0, 30, 0), rot=(-50, -35, 0),
                  directionalLight={"ambient": 0.5, "color": [1.0, 0.94, 0.84], "direction": [-0.5, -0.7, -0.5], "intensity": 3.3}))
    grp_env = ent("ENV"); grp_karts = ent("KARTS"); grp_items = ent("ITEMS")
    es += [grp_env, grp_karts, grp_items]

    # コースの見た目（Blender の glb をワールドの原点に置くだけ）
    for m in COURSE_MODELS:
        es.append(ent("ENV_" + m, grp_env, model=f"models/kart/{m}.glb"))

    # 信号（GroveKart.lua が色を変える球）
    p, fwd, right, yaw0 = frame_at(fine, 0)
    for k in range(3):
        off = (k - 1) * 2.2
        es.append(ent(f"KART_Signal{k + 1}", grp_env, prim="sphere",
                      pos=(p[0] + right[0] * off - fwd[0] * 0.5, 7.2, p[1] + right[1] * off - fwd[1] * 0.5),
                      scale=(1.0, 1.0, 1.0), color=(0.15, 0.03, 0.03), rough=0.3))

    # アイテムボックス（3 か所 × 4 個）
    for row, frac in enumerate(ITEM_FRACS):
        p, fwd, right, _ = frame_at(fine, int(L * frac))
        for c in range(4):
            off = (c - 1.5) * 2.6
            es.append(ent(f"KART_Item{row}{c}", grp_items, model="models/kart/itembox.glb",
                          pos=(p[0] + right[0] * off, 1.2, p[1] + right[1] * off), scale=(1.0, 1.0, 1.0)))

    # カート（親 = GroveKart.lua が動かす単位、子 = 見た目の glb）
    for name, model in KART_MODELS.items():
        root = ent(name, grp_karts, pos=(0, 0, 0))
        es.append(root)
        es.append(ent(name + "_Model", root, model=f"models/kart/{model}.glb"))

    es.append(ent("KART_Cam", None, pos=(0, 4, -38), camera={"fovDegrees": 62.0, "isActive": True}))
    es.append(ent("KART_Game", None, pos=(0, 0, 0), luaScript={"enabled": True, "scriptPath": "components/GroveKart.lua"}))

    scene["entities"] = es
    path = os.path.join(ASSETS, "scenes", "kart.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(scene, f, ensure_ascii=False, indent=None)
    print(f"kart.json: {len(es)} エンティティ / コース 1 周 {total:.0f} m / 中心線 {L} 点 / layout.json {len(one)} 点")


if __name__ == "__main__":
    main()
