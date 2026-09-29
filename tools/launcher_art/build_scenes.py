#!/usr/bin/env python3
"""ランチャー用の画像（テンプレートのカード画像 + 背景ヒーロー）をエンジンで描くための「撮影用シーン」を作る。

  python tools/launcher_art/build_scenes.py --out <使い捨てプロジェクトのフォルダ>

出力（<out>/ に無ければプロジェクトごと作る）:
  <out>/<name>.dx12proj
  <out>/assets/scenes/launcher_{fps,tps,2d,empty,hero}.json

すべてプリミティブ（box / sphere / plane）+ ライト + ポストだけで組んだ自作シーン（外部素材なし・著作権の心配なし）。
撮影は tools/launcher_art/render.mjs（MCP）→ 仕上げは tools/launcher_art/finalize.py。手順は docs/LAUNCHER.md。
"""
import argparse
import json
import math
import os

# ---------------------------------------------------------------- 部品


def ent(name, prim=None, pos=(0, 0, 0), rot=(0, 0, 0), scale=(1, 1, 1), color=None,
        emissive=None, glow=0.0, metallic=None, roughness=None, size=None, **extra):
    e = {"name": name}
    if prim:
        e["primitive"] = prim
        if size is not None:
            e["primitiveSize"] = size
    if color is not None:
        e["color"] = list(color)
    mat = {}
    if metallic is not None:
        mat["metallic"] = metallic
    if roughness is not None:
        mat["roughness"] = roughness
    if emissive is not None:
        mat["emissiveColor"] = list(emissive)
        mat["emissiveIntensity"] = glow
    if mat:
        e["material"] = mat
    e["transform"] = {"position": list(pos), "rotation": list(rot), "scale": list(scale)}
    e.update(extra)
    return e


def point(name, pos, color, intensity, rng, shadows=False):
    return {"name": name,
            "pointLight": {"color": list(color), "intensity": intensity, "range": rng, "castShadows": shadows},
            "transform": {"position": list(pos), "rotation": [0, 0, 0], "scale": [1, 1, 1]}}


def spot(name, pos, direction, color, intensity, rng, inner=20, outer=38, shadows=False):
    return {"name": name,
            "spotLight": {"color": list(color), "intensity": intensity, "range": rng, "direction": list(direction),
                          "innerConeDeg": inner, "outerConeDeg": outer, "castShadows": shadows},
            "transform": {"position": list(pos), "rotation": [0, 0, 0], "scale": [1, 1, 1]}}


def sun(direction, color, intensity, ambient, rot=(55, -30, 0)):
    return {"name": "Sun",
            "directionalLight": {"direction": list(direction), "color": list(color), "intensity": intensity, "ambient": ambient},
            "transform": {"position": [0, 20, 0], "rotation": list(rot), "scale": [1, 1, 1]}}


def camera(pos, rot, fov=60.0, ortho=None):
    c = {"fovDegrees": fov, "nearClip": 0.05, "farClip": 900.0, "isActive": True}
    if ortho is not None:
        c["projection"] = 1
        c["orthoSize"] = ortho
    return {"name": "MainCamera", "camera": c,
            "transform": {"position": list(pos), "rotation": list(rot), "scale": [1, 1, 1]}}


def look_rot(pos, target):
    """位置→注視点から Transform の回転（度）を求める。+Z 前方 / Y 上。rot = (pitch, yaw, 0)。"""
    dx, dy, dz = target[0] - pos[0], target[1] - pos[1], target[2] - pos[2]
    yaw = math.degrees(math.atan2(dx, dz))
    pitch = -math.degrees(math.atan2(dy, math.hypot(dx, dz)))
    return (pitch, yaw, 0.0)


def scene(entities, post=None, fog=None, ssr=None, ssao=True, shadows=True, sky=None, taa=None, extra=None):
    s = {"entities": entities, "shadows": shadows, "ssao": {"enabled": ssao}}
    s["postProcess"] = dict({"enabled": True, "fxaaOn": True}, **(post or {}))
    if fog:
        s["volumetricFog"] = dict({"enabled": True}, **fog)
    if ssr:
        s["ssr"] = dict({"enabled": True}, **ssr)
    if sky is not None:
        s["skybox"] = sky
    if taa:
        s["taa"] = {"enabled": True}
    if extra:
        s.update(extra)
    return s


# ---------------------------------------------------------------- FPS: 暗い廊下

def build_fps():
    E = []
    cam = (0.0, 1.72, -7.0)
    tgt = (0.0, 1.55, 30.0)
    L, W, H = 70.0, 6.0, 4.4
    dark = (0.045, 0.05, 0.065)
    E.append(sun((0.0, -1.0, 0.2), (0.5, 0.6, 0.9), 0.04, 0.05))
    E.append(camera(cam, look_rot(cam, tgt), 70.0))
    # 床（濡れた金属）・天井・両壁
    E.append(ent("Floor", "box", (0, -0.15, 30), scale=(W, 0.3, L), color=(0.05, 0.06, 0.08), metallic=0.7, roughness=0.16))
    E.append(ent("Ceiling", "box", (0, H + 0.15, 30), scale=(W + 0.8, 0.3, L), color=dark, roughness=0.7))
    for sx in (-1, 1):
        E.append(ent("Wall_%d" % sx, "box", (sx * (W / 2 + 0.15), H / 2, 30), scale=(0.3, H, L), color=dark, roughness=0.55))
    # 壁のパネルと柱（4m ごと）+ 縁の発光ライン
    z = -4.0
    i = 0
    while z < 60.0:
        for sx in (-1, 1):
            E.append(ent("Rib_%d_%d" % (i, sx), "box", (sx * (W / 2 - 0.12), H / 2, z), scale=(0.34, H, 0.5),
                         color=(0.1, 0.115, 0.15), metallic=0.5, roughness=0.4))
            E.append(ent("Panel_%d_%d" % (i, sx), "box", (sx * (W / 2 - 0.04), 1.9, z + 2.0), scale=(0.1, 2.6, 2.6),
                         color=(0.07, 0.08, 0.105), metallic=0.4, roughness=0.5))
            E.append(ent("PanelEdge_%d_%d" % (i, sx), "box", (sx * (W / 2 - 0.09), 0.62, z + 2.0), scale=(0.05, 0.05, 2.5),
                         color=(0.1, 0.5, 1.0), emissive=(0.15, 0.55, 1.0), glow=5.0))
        # 天井の帯状ライト
        E.append(ent("CeilLight_%d" % i, "box", (0, H - 0.06, z + 2.0), scale=(0.7, 0.08, 2.6), color=(0.8, 0.9, 1.0),
                     emissive=(0.6, 0.78, 1.0), glow=3.2))
        if i % 2 == 0:
            E.append(point("CeilPoint_%d" % i, (0, H - 0.5, z + 2.0), (0.45, 0.65, 1.0), 6.0, 9.0))
        z += 4.0
        i += 1
    # 手前は少し暗く・奥は開口部の逆光で明るく
    E.append(ent("DoorGlow", "box", (0, 2.0, 62.0), scale=(4.4, 4.0, 0.2), color=(1.0, 0.8, 0.5),
                 emissive=(1.0, 0.62, 0.3), glow=5.0))
    E.append(point("DoorLight", (0, 2.3, 58.0), (1.0, 0.6, 0.28), 34.0, 26.0))
    E.append(point("DoorLight2", (0, 2.3, 48.0), (1.0, 0.55, 0.25), 12.0, 16.0))
    # 逆光に浮かぶ人影
    zz = 40.0
    E.append(ent("EnemyBody", "box", (0.3, 1.15, zz), scale=(0.62, 0.95, 0.36), color=(0.02, 0.02, 0.025), roughness=0.9))
    E.append(ent("EnemyLegL", "box", (0.05, 0.42, zz), scale=(0.25, 0.86, 0.26), color=(0.02, 0.02, 0.025), roughness=0.9))
    E.append(ent("EnemyLegR", "box", (0.55, 0.42, zz), scale=(0.25, 0.86, 0.26), color=(0.02, 0.02, 0.025), roughness=0.9))
    E.append(ent("EnemyHead", "sphere", (0.3, 1.86, zz), scale=(0.34, 0.38, 0.34), color=(0.02, 0.02, 0.025), roughness=0.9))
    E.append(ent("EnemyArmL", "box", (-0.12, 1.2, zz + 0.1), rot=(-20, 0, 8), scale=(0.16, 0.8, 0.18), color=(0.02, 0.02, 0.025), roughness=0.9))
    E.append(ent("EnemyArmR", "box", (0.72, 1.28, zz + 0.25), rot=(-65, 0, -6), scale=(0.16, 0.8, 0.18), color=(0.02, 0.02, 0.025), roughness=0.9))
    E.append(ent("EnemyEyes", "box", (0.3, 1.9, zz - 0.2), scale=(0.24, 0.05, 0.03), color=(1, 0.2, 0.15), emissive=(1, 0.1, 0.05), glow=8.0))
    # 通路の障害物（木箱・ドラム缶代わりの箱）
    for k, (x, zc, s, r) in enumerate([(-2.0, 8.0, 1.3, 15), (-2.15, 9.4, 0.9, -10), (2.1, 14.0, 1.5, 8), (-1.9, 20.0, 1.2, 30), (2.0, 26.0, 1.4, -18)]):
        E.append(ent("Crate_%d" % k, "box", (x, s / 2, zc), rot=(0, r, 0), scale=(s, s, s), color=(0.24, 0.21, 0.15),
                     metallic=0.15, roughness=0.6))
        E.append(ent("CrateBand_%d" % k, "box", (x, s * 0.5, zc), rot=(0, r, 0), scale=(s * 1.03, s * 0.14, s * 1.03),
                     color=(0.1, 0.11, 0.13), metallic=0.8, roughness=0.3))
    # 赤い警告灯（奥）
    for zr in (24.0, 40.0):
        E.append(ent("Warn_%d" % int(zr), "box", (-2.7, 3.2, zr), scale=(0.16, 0.16, 0.16), color=(1, 0.1, 0.1),
                     emissive=(1.0, 0.08, 0.05), glow=10.0))
        E.append(point("WarnLight_%d" % int(zr), (-2.4, 3.1, zr), (1.0, 0.1, 0.06), 10.0, 8.0))
    # 手前の武器（カメラ基準で配置 = 一人称の見え方）
    def cam_pt(dx, dy, dz):
        return (cam[0] + dx, cam[1] + dy, cam[2] + dz)
    gun_col = (0.16, 0.175, 0.21)
    E.append(ent("Gun_Body", "box", cam_pt(0.30, -0.27, 0.62), rot=(2, -4, 0), scale=(0.085, 0.13, 0.5), color=gun_col, metallic=0.85, roughness=0.32))
    E.append(ent("Gun_Slide", "box", cam_pt(0.30, -0.205, 0.66), rot=(2, -4, 0), scale=(0.07, 0.05, 0.42), color=(0.11, 0.12, 0.15), metallic=0.9, roughness=0.25))
    E.append(ent("Gun_Barrel", "box", cam_pt(0.29, -0.215, 0.95), rot=(2, -4, 0), scale=(0.045, 0.045, 0.24), color=(0.03, 0.03, 0.04), metallic=0.9, roughness=0.3))
    E.append(ent("Gun_Grip", "box", cam_pt(0.32, -0.40, 0.48), rot=(-14, -4, 0), scale=(0.075, 0.2, 0.11), color=(0.04, 0.045, 0.055), roughness=0.7))
    E.append(ent("Gun_Sight", "box", cam_pt(0.30, -0.17, 0.5), rot=(2, -4, 0), scale=(0.02, 0.03, 0.03), color=(0.1, 0.5, 1.0), emissive=(0.1, 0.5, 1.0), glow=8.0))
    E.append(ent("Gun_Strip", "box", cam_pt(0.262, -0.255, 0.68), rot=(2, -4, 0), scale=(0.006, 0.02, 0.3), color=(0.1, 0.5, 1.0), emissive=(0.1, 0.5, 1.0), glow=9.0))
    E.append(point("Gun_Fill", cam_pt(0.1, 0.1, 0.3), (0.55, 0.7, 1.0), 2.6, 1.6))
    post = {"bloomOn": True, "bloom": 0.85, "bloomThreshold": 0.85, "bloomRadius": 0.9,
            "vignetteOn": True, "vignette": 0.45, "vignetteRadius": 0.85, "vignetteSoftness": 0.6,
            "saturationOn": True, "saturation": 1.1, "contrastOn": True, "contrast": 1.12, "exposureOn": True, "exposure": 1.7,
            "chromaticOn": True, "chromatic": 0.12, "lensflareOn": False}
    fog = {"density": 0.028, "anisotropy": 0.35, "heightFalloff": 0.02, "distance": 60.0, "lightScattering": 1.0,
           "ambient": [0.02, 0.03, 0.05], "sunIntensity": 0.0}
    return scene(E, post, fog, ssr={"intensity": 0.75, "maxDistance": 40.0}, sky={"envMapPath": "", "iblIntensity": 0.05, "skyboxIntensity": 0.0, "drawSkybox": False})


# ---------------------------------------------------------------- TPS: 屋外

def build_tps():
    E = []
    cam = (-3.4, 3.1, -9.6)
    tgt = (0.6, 1.5, 0.0)
    E.append(sun((-0.5, -0.55, 0.65), (1.0, 0.86, 0.62), 2.6, 0.5, rot=(30, 20, 0)))
    E.append(camera(cam, look_rot(cam, tgt), 55.0))
    # 地面（草）+ 小道
    E.append(ent("Ground", "box", (0, -0.5, 30), scale=(180, 1, 180), color=(0.18, 0.42, 0.14), roughness=0.95))
    E.append(ent("Path", "box", (0.5, 0.01, 12), rot=(0, -6, 0), scale=(3.4, 0.04, 50), color=(0.62, 0.5, 0.32), roughness=0.95))
    # 丘（つぶした球）
    hills = [(-38, -1, 46, 34, 13, 28, (0.16, 0.4, 0.13)), (30, -1, 60, 40, 17, 32, (0.14, 0.36, 0.12)),
             (2, -2, 90, 70, 26, 40, (0.13, 0.32, 0.12)), (-70, -2, 70, 60, 20, 36, (0.12, 0.3, 0.11)),
             (62, -1, 24, 26, 7, 20, (0.17, 0.42, 0.14))]
    for k, (x, y, z, sx, sy, sz, c) in enumerate(hills):
        E.append(ent("Hill_%d" % k, "sphere", (x, y, z), scale=(sx, sy, sz), color=c, roughness=0.95))
    # 木（幹 + 三段の葉）
    trees = [(-7.0, 4.0), (-9.5, 11.0), (8.5, 7.0), (10.5, 15.0), (-6.0, 20.0), (7.0, 26.0), (-12.5, 5.0), (13.0, 3.0), (-8.0, 30.0), (9.0, 34.0)]
    for k, (x, z) in enumerate(trees):
        s = 1.0 + 0.25 * ((k * 37) % 5) / 4.0
        E.append(ent("Trunk_%d" % k, "box", (x, 1.0 * s, z), scale=(0.42 * s, 2.0 * s, 0.42 * s), color=(0.32, 0.2, 0.1), roughness=0.9))
        for j, (dy, r) in enumerate([(2.4, 1.55), (3.5, 1.2), (4.4, 0.8)]):
            E.append(ent("Leaf_%d_%d" % (k, j), "sphere", (x, dy * s, z), scale=(r * 2.0 * s, r * 1.7 * s, r * 2.0 * s),
                         color=(0.13 + 0.02 * j, 0.5 + 0.03 * j, 0.16), roughness=0.9))
    # 岩・花
    for k, (x, z, s) in enumerate([(3.2, 3.0, 0.9), (-3.4, 7.0, 0.7), (4.0, 12.0, 1.1), (-4.2, 15.0, 0.8)]):
        E.append(ent("Rock_%d" % k, "box", (x, s * 0.35, z), rot=(8, 25 * k, 5), scale=(s * 1.3, s * 0.8, s), color=(0.42, 0.42, 0.44), roughness=0.9))
    for k in range(18):
        x = -5.0 + (k * 7.31) % 10.0
        z = 2.0 + (k * 3.9) % 26.0
        if abs(x - 0.5) < 2.2:
            x += 3.4
        col = [(1.0, 0.85, 0.25), (1.0, 0.45, 0.65), (0.95, 0.95, 1.0)][k % 3]
        E.append(ent("Flower_%d" % k, "sphere", (x, 0.14, z), scale=(0.2, 0.2, 0.2), color=col, emissive=col, glow=0.6, roughness=0.8))
    # コインの列（金・発光）
    for k in range(9):
        z = 2.2 + k * 2.0
        x = 0.5 + 0.9 * math.sin(k * 0.7)
        E.append(ent("Coin_%d" % k, "sphere", (x, 1.05 + 0.18 * math.sin(k), z), rot=(0, 0, 0), scale=(0.62, 0.62, 0.12),
                     color=(1.0, 0.8, 0.15), metallic=0.9, roughness=0.25, emissive=(1.0, 0.7, 0.1), glow=1.6))
    # ゴールの旗
    E.append(ent("FlagPole", "box", (0.6, 3.2, 30.0), scale=(0.16, 6.4, 0.16), color=(0.9, 0.9, 0.92), metallic=0.7, roughness=0.3))
    E.append(ent("Flag", "box", (1.6, 5.5, 30.0), scale=(2.0, 1.2, 0.06), color=(1.0, 0.3, 0.25), emissive=(1.0, 0.25, 0.2), glow=1.2))
    E.append(ent("GoalRing", "sphere", (0.6, 0.03, 30.0), scale=(4.6, 0.06, 4.6), color=(0.3, 0.9, 1.0), emissive=(0.3, 0.9, 1.0), glow=3.0))
    # キャラクター（後ろ姿・少し斜め）
    P = (0.0, 0.0, -1.2)
    def ch(name, prim, dx, dy, dz, sx, sy, sz, col, r=(0, 0, 0), **kw):
        return ent(name, prim, (P[0] + dx, P[1] + dy, P[2] + dz), rot=r, scale=(sx, sy, sz), color=col, **kw)
    skin, shirt, pants, pack = (0.98, 0.78, 0.62), (0.95, 0.32, 0.28), (0.16, 0.22, 0.42), (0.22, 0.62, 0.7)
    E += [
        ch("Hero_LegL", "box", -0.13, 0.42, 0.0, 0.2, 0.85, 0.24, pants, r=(-8, 0, 0)),
        ch("Hero_LegR", "box", 0.13, 0.42, 0.06, 0.2, 0.85, 0.24, pants, r=(10, 0, 0)),
        ch("Hero_Torso", "box", 0.0, 1.18, 0.0, 0.58, 0.72, 0.34, shirt),
        ch("Hero_Head", "sphere", 0.0, 1.74, 0.0, 0.5, 0.52, 0.5, skin),
        ch("Hero_Hair", "sphere", 0.0, 1.86, -0.04, 0.53, 0.42, 0.52, (0.2, 0.12, 0.08)),
        ch("Hero_ArmL", "box", -0.4, 1.2, 0.05, 0.16, 0.66, 0.2, shirt, r=(-15, 0, 6)),
        ch("Hero_ArmR", "box", 0.4, 1.22, 0.0, 0.16, 0.66, 0.2, shirt, r=(20, 0, -6)),
        ch("Hero_Pack", "box", 0.0, 1.22, -0.26, 0.46, 0.55, 0.22, pack),
        ch("Hero_PackStrip", "box", 0.0, 1.22, -0.375, 0.34, 0.06, 0.03, (1, 1, 1), emissive=(0.4, 0.9, 1.0), glow=2.0),
    ]
    post = {"bloomOn": True, "bloom": 0.55, "bloomThreshold": 0.9, "vignetteOn": True, "vignette": 0.3,
            "saturationOn": True, "saturation": 1.18, "warmthOn": True, "warmth": 0.08, "contrastOn": True, "contrast": 1.06,
            "godraysOn": True, "grIntensity": 0.35, "grDensity": 0.7, "grDecay": 0.92}
    fog = {"density": 0.006, "anisotropy": 0.6, "heightFalloff": 0.05, "distance": 220.0, "lightScattering": 1.0,
           "ambient": [0.28, 0.36, 0.5], "sunIntensity": 1.0, "albedo": [0.85, 0.9, 1.0]}
    return scene(E, post, fog, ssao=True, sky={"envMapPath": "__procedural_sky__", "iblIntensity": 0.9, "skyboxIntensity": 1.1, "drawSkybox": True})


# ---------------------------------------------------------------- 2D: ドット絵風

def build_2d():
    E = []
    # 正射投影で真横から。ポストのピクセル化 + ポスタライズでドット絵の質感にする。
    E.append(ent("Sun", None, (0, 20, -20), rot=(0, 0, 0), directionalLight={"direction": [0.0, -0.3, 1.0], "color": [1, 1, 1], "intensity": 0.9, "ambient": 0.5}))
    E.append(camera((7.5, 4.6, -30.0), (0, 0, 0), 60.0, ortho=7.2))
    bands = [(-2.0, (0.72, 0.88, 1.0)), (1.5, (0.58, 0.8, 1.0)), (5.0, (0.44, 0.7, 0.98)), (8.5, (0.34, 0.6, 0.96)), (12.0, (0.27, 0.52, 0.93))]
    for k, (y, c) in enumerate(bands):
        E.append(ent("Sky_%d" % k, "box", (7.5, y, 14.0), scale=(70, 3.8, 0.2), color=c, roughness=1.0))
    E.append(ent("Under", "box", (7.5, -4.5, 0.0), scale=(70, 4.0, 4.0), color=(0.42, 0.25, 0.16), roughness=1.0))
    # 遠景の丘（ピクセルの段々）
    hill = [(-10, 3, 3.6), (-7, 3, 4.2), (-4, 3, 3.6), (-1, 4, 5.0), (3, 5, 6.2), (8, 4, 5.0), (12, 6, 7.6), (17, 4, 5.0), (21, 3, 3.6), (25, 3, 4.6), (28, 3, 3.6)]
    for k, (x, w, h) in enumerate(hill):
        E.append(ent("Hill_%d" % k, "box", (x + 0.5, h / 2 - 0.6, 10.0), scale=(w * 1.6, h, 0.4), color=(0.37, 0.72, 0.56), roughness=1.0))
        E.append(ent("HillTop_%d" % k, "box", (x + 0.5, h - 0.6 - 0.25, 9.8), scale=(w * 1.6, 0.5, 0.4), color=(0.46, 0.82, 0.62), roughness=1.0))
    # 雲
    for k, (x, y, s) in enumerate([(-1.5, 9.4, 1.0), (6.5, 10.2, 1.3), (14.5, 9.0, 1.1), (19.5, 10.6, 0.9)]):
        for j, (dx, dy, w, h) in enumerate([(0, 0, 3.6, 0.9), (0.5, 0.7, 2.4, 0.8), (-0.6, 0.6, 1.4, 0.6)]):
            E.append(ent("Cloud_%d_%d" % (k, j), "box", (x + dx * s, y + dy * s, 8.0), scale=(w * s, h * s, 0.3), color=(1, 1, 1), roughness=1.0))
    # 地面（草 + 土のタイル）: 1 ユニットごとに市松
    def tile(x, y, top, name):
        E.append(ent(name + "_d", "box", (x, y, 0.0), scale=(1.0, 1.0, 1.6), color=(0.55, 0.33, 0.2) if (int(x) + int(y)) % 2 else (0.5, 0.29, 0.18), roughness=1.0))
        if top:
            E.append(ent(name + "_g", "box", (x, y + 0.38, -0.05), scale=(1.0, 0.3, 1.7), color=(0.36, 0.78, 0.22), roughness=1.0))
    gx = [(-9, 6), (8, 14), (17, 30)]
    idx = 0
    for a, b in gx:
        for x in range(a, b):
            tile(x + 0.5, 0.0, True, "G%d" % idx)
            tile(x + 0.5, -1.0, False, "H%d" % idx)
            idx += 1
    # 浮島・ブロック
    for k, (x, y, n) in enumerate([(5.5, 3.0, 3), (14.5, 4.4, 3), (9.5, 6.2, 2)]):
        for i in range(n):
            E.append(ent("Plat_%d_%d" % (k, i), "box", (x + i, y, 0.0), scale=(1.0, 0.7, 1.6), color=(0.62, 0.45, 0.86) if i % 2 else (0.72, 0.55, 0.95), roughness=1.0))
    for k, (x, y) in enumerate([(3.5, 2.0), (4.5, 2.0)]):
        E.append(ent("Block_%d" % k, "box", (x, y, 0.0), scale=(1.0, 1.0, 1.6), color=(0.96, 0.62, 0.2), roughness=1.0))
        E.append(ent("BlockQ_%d" % k, "box", (x, y, -0.85), scale=(0.35, 0.35, 0.1), color=(1, 0.95, 0.5), roughness=1.0))
    # コイン
    for k, (x, y) in enumerate([(6.0, 4.3), (7.0, 4.3), (8.0, 4.3), (14.8, 5.8), (15.8, 5.8), (10.0, 7.6), (11.0, 7.6)]):
        E.append(ent("Coin_%d" % k, "box", (x, y, -0.2), scale=(0.62, 0.78, 0.2), color=(1.0, 0.85, 0.15), roughness=1.0))
        E.append(ent("CoinS_%d" % k, "box", (x - 0.1, y + 0.1, -0.35), scale=(0.16, 0.36, 0.1), color=(1.0, 1.0, 0.72), roughness=1.0))
    # トゲ
    for k, x in enumerate([11.5, 12.5]):
        for j in range(3):
            E.append(ent("Spike_%d_%d" % (k, j), "box", (x - 0.28 + j * 0.28, 0.72, -0.3), rot=(0, 0, 45), scale=(0.3, 0.3, 0.4), color=(0.85, 0.85, 0.92), roughness=1.0))
    # ゴール旗
    E.append(ent("Pole", "box", (21.5, 3.4, 0.0), scale=(0.18, 5.6, 0.3), color=(0.92, 0.92, 0.95), roughness=1.0))
    E.append(ent("Flag", "box", (22.3, 5.4, 0.0), scale=(1.6, 1.0, 0.2), color=(1.0, 0.3, 0.3), roughness=1.0))
    E.append(ent("FlagTop", "sphere", (21.5, 6.3, 0.0), scale=(0.4, 0.4, 0.4), color=(1.0, 0.85, 0.2), roughness=1.0))
    # プレイヤー（ドット絵 12x14 の縮小版を箱で）
    px, py = 1.6, 1.0
    pix = [
        "....rrrr....", "...rrrrrr...", "...ssssss...", "..sesssses..", "..ssssssss..", "...ssmms....",
        "..bbbbbbbb..", ".bbbwbbwbbb.", ".ss.bbbb.ss.", ".ss.bbbb.ss.", "...bbbbbb...", "...nn..nn...", "...nn..nn...", "..kkk..kkk..",
    ]
    pal = {"r": (0.9, 0.2, 0.22), "s": (1.0, 0.8, 0.62), "e": (0.1, 0.1, 0.16), "m": (0.7, 0.3, 0.25), "b": (0.2, 0.42, 0.95),
           "w": (1.0, 1.0, 1.0), "n": (0.15, 0.2, 0.55), "k": (0.35, 0.2, 0.12)}
    u = 0.125
    for j, row in enumerate(pix):
        for i, ch_ in enumerate(row):
            if ch_ == ".":
                continue
            E.append(ent("Px_%d_%d" % (j, i), "box", (px + (i - 6) * u * 1.0, py + (len(pix) - 1 - j) * u * 1.0 - 0.2, -0.6),
                          scale=(u * 1.02, u * 1.02, 0.2), color=pal[ch_], roughness=1.0))
    post = {"pixelizeOn": True, "pixelSize": 5.0, "posterizeOn": True, "posterize": 20, "saturationOn": True, "saturation": 1.25, "contrastOn": True, "contrast": 1.12,
            "vignetteOn": True, "vignette": 0.2, "bloomOn": False, "fxaaOn": False, "scanlineOn": False}
    return scene(E, post, None, ssao=False, shadows=False, sky={"envMapPath": "", "iblIntensity": 0.4, "skyboxIntensity": 0.0, "drawSkybox": False})


# ---------------------------------------------------------------- 空: グリッド

def build_empty():
    E = []
    cam = (5.2, 2.6, -7.5)
    tgt = (0.0, 0.5, 0.0)
    E.append(sun((-0.3, -1.0, 0.45), (0.85, 0.9, 1.0), 1.0, 0.16))
    E.append(camera(cam, look_rot(cam, tgt), 44.0))
    E.append(ent("Grid", "plane", (0, 0, 0), size=200.0, color=(0.03, 0.035, 0.05), metallic=0.5, roughness=0.3))
    # グリッドの線（薄い発光ライン）: 1m 間隔を手前 ±14m だけ
    for i in range(-14, 15):
        major = (i % 5 == 0)
        c = (0.16, 0.5, 1.0) if major else (0.1, 0.24, 0.42)
        g = 2.2 if major else 0.9
        E.append(ent("LineX_%d" % i, "box", (i, 0.004, 4.0), scale=(0.028 if major else 0.014, 0.004, 40.0), color=c, emissive=c, glow=g))
        E.append(ent("LineZ_%d" % i, "box", (0.0, 0.004, i * 1.0 + 4.0), scale=(40.0, 0.004, 0.028 if major else 0.014), color=c, emissive=c, glow=g))
    # 中央のキューブ（フレームだけ光る）
    E.append(ent("Cube", "box", (0.0, 0.5, 0.0), rot=(0, 28, 0), scale=(1.0, 1.0, 1.0), color=(0.62, 0.68, 0.8), metallic=0.3, roughness=0.35))
    for k, (dx, dz) in enumerate([(0.5, 0.5), (0.5, -0.5), (-0.5, 0.5), (-0.5, -0.5)]):
        a = math.radians(28)
        x = dx * math.cos(a) + dz * math.sin(a)
        z = -dx * math.sin(a) + dz * math.cos(a)
        E.append(ent("CubeEdge_%d" % k, "box", (x, 0.5, z), rot=(0, 28, 0), scale=(0.028, 1.03, 0.028), color=(0.3, 0.7, 1.0), emissive=(0.3, 0.7, 1.0), glow=5.0))
    E.append(point("CubeGlow", (0.0, 1.4, 0.0), (0.3, 0.6, 1.0), 6.0, 7.0))
    E.append(ent("Orb", "sphere", (-2.6, 0.32, 1.8), scale=(0.5, 0.5, 0.5), color=(0.4, 0.8, 1.0), emissive=(0.3, 0.75, 1.0), glow=4.5))
    E.append(point("OrbLight", (-2.6, 0.7, 1.8), (0.3, 0.7, 1.0), 4.0, 5.0))
    post = {"bloomOn": True, "bloom": 0.8, "bloomThreshold": 0.8, "vignetteOn": True, "vignette": 0.5, "vignetteRadius": 0.8,
            "saturationOn": True, "saturation": 1.05}
    fog = {"density": 0.012, "anisotropy": 0.2, "heightFalloff": 0.03, "distance": 80.0, "ambient": [0.02, 0.05, 0.1], "sunIntensity": 0.0}
    return scene(E, post, fog, ssr={"intensity": 0.6, "maxDistance": 30.0}, sky={"envMapPath": "", "iblIntensity": 0.06, "skyboxIntensity": 0.0, "drawSkybox": False})


# ---------------------------------------------------------------- ヒーロー: 夜のプラザ

def build_hero():
    E = []
    cam = (-3.0, 2.4, -36.0)
    tgt = (2.0, 7.0, 10.0)
    E.append(sun((0.2, -0.6, 0.7), (0.5, 0.62, 1.0), 0.18, 0.06))
    E.append(camera(cam, look_rot(cam, tgt), 58.0))
    # 濡れた黒い床（反射）
    E.append(ent("Plaza", "box", (0, -0.25, 30), scale=(160, 0.5, 200), color=(0.03, 0.035, 0.05), metallic=0.85, roughness=0.12))
    # 床の発光ライン（放射状に奥へ）
    for i in range(-6, 7):
        c = (0.12, 0.5, 1.0)
        E.append(ent("Lane_%d" % i, "box", (i * 3.0, 0.005, 26), scale=(0.05, 0.01, 90.0), color=c, emissive=c, glow=0.9 if i % 3 == 0 else 0.35))
    for k in range(10):
        E.append(ent("Cross_%d" % k, "box", (0, 0.005, -8 + k * 9.0), scale=(60.0, 0.01, 0.05), color=(0.1, 0.4, 0.9), emissive=(0.1, 0.4, 0.9), glow=0.4))
    # モノリス（高さ違い）+ 縁の発光
    mono = [(-14, 8, 3.2, 26.0), (-8, 18, 2.6, 16.0), (-19, 26, 3.6, 34.0), (9, 12, 3.0, 22.0), (15, 22, 3.4, 30.0), (20, 8, 2.4, 14.0),
            (-4, 34, 4.4, 42.0), (5, 40, 4.6, 52.0), (-12, 46, 3.6, 40.0), (14, 48, 4.0, 46.0), (0, 56, 5.0, 62.0)]
    for k, (x, z, w, h) in enumerate(mono):
        d = w * 0.9
        E.append(ent("Mono_%d" % k, "box", (x, h / 2, z), scale=(w, h, d), color=(0.035, 0.04, 0.055), metallic=0.6, roughness=0.28))
        e_c = (0.15, 0.55, 1.0) if k % 3 else (0.35, 0.8, 1.0)
        E.append(ent("MonoEdge_%d" % k, "box", (x + w / 2 - 0.02, h / 2, z - d / 2 - 0.01), scale=(0.07, h, 0.05), color=e_c, emissive=e_c, glow=1.6))
        E.append(ent("MonoEdgeL_%d" % k, "box", (x - w / 2 + 0.02, h / 2, z - d / 2 - 0.01), scale=(0.07, h, 0.05), color=e_c, emissive=e_c, glow=1.0))
        E.append(ent("MonoBand_%d" % k, "box", (x, h * 0.72, z - d / 2 - 0.01), scale=(w * 0.8, 0.09, 0.05), color=e_c, emissive=e_c, glow=1.2))
    # 中央上空に浮かぶ光るキューブ（Uno のロゴのモチーフ）
    E.append(ent("Core", "box", (2.0, 15.0, 28.0), rot=(35, 45, 0), scale=(6.0, 6.0, 6.0), color=(0.05, 0.1, 0.2), metallic=0.9, roughness=0.15, emissive=(0.15, 0.5, 1.0), glow=0.5))
    for k, (dx, dy) in enumerate([(1, 1), (1, -1), (-1, 1), (-1, -1)]):
        E.append(ent("CoreEdge_%d" % k, "box", (2.0 + dx * 3.0 * 0.7071, 15.0 + dy * 3.0, 28.0), rot=(35, 45, 0), scale=(0.16, 0.16, 6.4),
                     color=(0.3, 0.8, 1.0), emissive=(0.2, 0.7, 1.0), glow=3.0))
    E.append(ent("CoreHeart", "sphere", (2.0, 15.0, 28.0), scale=(3.0, 3.0, 3.0), color=(0.5, 0.85, 1.0), emissive=(0.25, 0.65, 1.0), glow=3.5))
    E.append(point("CoreLight", (2.0, 15.0, 26.0), (0.25, 0.6, 1.0), 70.0, 50.0))
    E.append(point("CoreLightLow", (2.0, 2.5, 20.0), (0.2, 0.5, 1.0), 9.0, 26.0))
    E.append(point("Rim1", (-10.0, 4.0, 4.0), (0.6, 0.3, 1.0), 10.0, 24.0))
    E.append(point("Rim2", (14.0, 5.0, 16.0), (0.1, 0.8, 0.9), 10.0, 26.0))
    post = {"bloomOn": True, "bloom": 0.45, "bloomThreshold": 1.0, "bloomRadius": 0.9, "vignetteOn": True, "vignette": 0.5, "vignetteRadius": 0.85,
            "saturationOn": True, "saturation": 1.15, "contrastOn": True, "contrast": 1.1, "chromaticOn": True, "chromatic": 0.12,
            "lensflareOn": False}
    fog = {"density": 0.006, "anisotropy": 0.5, "heightFalloff": 0.025, "distance": 140.0, "lightScattering": 0.8,
           "ambient": [0.02, 0.04, 0.09], "sunIntensity": 0.2, "albedo": [0.6, 0.75, 1.0]}
    return scene(E, post, fog, ssr={"intensity": 0.85, "maxDistance": 90.0}, sky={"envMapPath": "", "iblIntensity": 0.08, "skyboxIntensity": 0.0, "drawSkybox": False})


BUILDERS = {"fps": build_fps, "tps": build_tps, "2d": build_2d, "empty": build_empty, "hero": build_hero}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="撮影用の使い捨てプロジェクトのフォルダ（無ければ作る）")
    ap.add_argument("--only", default="", help="カンマ区切りで一部だけ（fps,tps,2d,empty,hero）")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(os.path.join(out, "assets", "scenes"), exist_ok=True)
    name = os.path.basename(out.rstrip("\\/"))
    proj = os.path.join(out, name + ".dx12proj")
    if not os.path.exists(proj):
        with open(proj, "w", encoding="utf-8") as f:
            json.dump({"name": name, "version": "1.0", "defaultScene": "scenes/launcher_empty.json", "assetsDir": "assets", "scriptsDir": "scripts"}, f, indent=2)
    os.makedirs(os.path.join(out, "scripts"), exist_ok=True)
    only = [x for x in a.only.split(",") if x] or list(BUILDERS)
    for k in only:
        path = os.path.join(out, "assets", "scenes", "launcher_%s.json" % k)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(BUILDERS[k](), f, indent=1)
        print("wrote", path)


if __name__ == "__main__":
    main()
