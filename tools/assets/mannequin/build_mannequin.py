# スキンドのマネキンと歩きのループ（1 秒 / 30fps）を Blender で自作して glb に書き出す。
# 外部の素材は使わない（すべてこのスクリプトが生成する）。CC0。
#
#   blender --background --factory-startup --python tools/assets/mannequin/build_mannequin.py -- <出力.glb> [<プレビュー用 PNG の接頭辞>]
#
# 座標: Blender の Z 上、正面が -Y。glTF 書き出しで Y 上・正面 +Z になる。身長 1.8m。
import math
import sys

import bpy
import bmesh
from mathutils import Euler, Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
out_path = argv[0] if argv else "mannequin.glb"
preview_prefix = argv[1] if len(argv) > 1 else ""

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 30
scene.frame_start = 0
scene.frame_end = 30  # 30 コマ目 = 0 コマ目（ループ。長さは 1.0 秒）

# ---- ボーン定義: 名前 -> (親, head, tail)。A ポーズ（腕は少し開く）----
B = {}


def bone(name, parent, head, tail):
    B[name] = (parent, Vector(head), Vector(tail))


bone("hips", None, (0, 0, 0.95), (0, 0, 1.07))
bone("spine", "hips", (0, 0, 1.07), (0, 0, 1.25))
bone("chest", "spine", (0, 0, 1.25), (0, 0, 1.45))
bone("neck", "chest", (0, 0, 1.45), (0, 0, 1.55))
bone("head", "neck", (0, 0, 1.55), (0, 0, 1.78))
for sd, sx in (("L", 1), ("R", -1)):
    sh = (sx * 0.19, 0, 1.40)
    el = (sx * 0.27, 0, 1.12)
    wr = (sx * 0.33, 0, 0.88)
    hd = (sx * 0.36, 0, 0.76)
    bone("upperarm." + sd, "chest", sh, el)
    bone("forearm." + sd, "upperarm." + sd, el, wr)
    bone("hand." + sd, "forearm." + sd, wr, hd)
    hp = (sx * 0.09, 0, 0.93)
    kn = (sx * 0.09, 0, 0.50)
    an = (sx * 0.09, 0, 0.09)
    bone("thigh." + sd, "hips", hp, kn)
    bone("shin." + sd, "thigh." + sd, kn, an)
    bone("foot." + sd, "shin." + sd, an, (sx * 0.09, -0.12, 0.05))
    bone("toe." + sd, "foot." + sd, (sx * 0.09, -0.12, 0.05), (sx * 0.09, -0.21, 0.03))

# ---- 体のパーツ（楕円体）----
parts = []


def ell(bone_name, center, radii):
    parts.append((bone_name, Vector(center), Vector(radii)))


ell("hips", (0, 0, 1.00), (0.15, 0.10, 0.10))
ell("spine", (0, 0, 1.16), (0.13, 0.09, 0.11))
ell("chest", (0, 0, 1.35), (0.18, 0.10, 0.13))
ell("neck", (0, 0, 1.51), (0.045, 0.045, 0.06))
ell("head", (0, 0, 1.68), (0.095, 0.11, 0.12))
for sd, sx in (("L", 1), ("R", -1)):
    ell("upperarm." + sd, (sx * 0.23, 0, 1.26), (0.045, 0.045, 0.16))
    ell("forearm." + sd, (sx * 0.30, 0, 1.00), (0.04, 0.04, 0.14))
    ell("hand." + sd, (sx * 0.345, 0, 0.82), (0.03, 0.045, 0.07))
    ell("thigh." + sd, (sx * 0.09, 0, 0.72), (0.075, 0.075, 0.24))
    ell("shin." + sd, (sx * 0.09, 0, 0.30), (0.055, 0.055, 0.23))
    ell("foot." + sd, (sx * 0.09, -0.05, 0.05), (0.05, 0.10, 0.045))
    ell("toe." + sd, (sx * 0.09, -0.17, 0.04), (0.045, 0.05, 0.03))

mesh = bpy.data.meshes.new("MannequinMesh")
bm = bmesh.new()
layer = bm.verts.layers.int.new("part")
for pi, (bname, c, r) in enumerate(parts):
    geom = bmesh.ops.create_uvsphere(bm, u_segments=14, v_segments=8, radius=1.0)
    for v in geom["verts"]:
        v.co = Vector((v.co.x * r.x, v.co.y * r.y, v.co.z * r.z)) + c
        v[layer] = pi
for f in bm.faces:
    f.smooth = True
bm.normal_update()
bm.to_mesh(mesh)
bm.free()
# 頂点の所属パーツは整数属性から読む（bmesh の index に頼らない）
vert_bone = [(i, parts[d.value][0]) for i, d in enumerate(mesh.attributes["part"].data)]
mesh.attributes.remove(mesh.attributes["part"])

obj = bpy.data.objects.new("Mannequin", mesh)
scene.collection.objects.link(obj)

# ---- マテリアル（無地のグレー。テクスチャなし）----
mat = bpy.data.materials.new("MannequinMat")
mat.use_nodes = True
bsdf = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
bsdf.inputs["Base Color"].default_value = (0.62, 0.64, 0.68, 1.0)
bsdf.inputs["Roughness"].default_value = 0.55
bsdf.inputs["Metallic"].default_value = 0.0
mesh.materials.append(mat)

# ---- アーマチュア ----
arm_data = bpy.data.armatures.new("MannequinArmature")
arm = bpy.data.objects.new("MannequinRig", arm_data)
scene.collection.objects.link(arm)
bpy.context.view_layer.objects.active = arm
arm.select_set(True)
bpy.ops.object.mode_set(mode="EDIT")
ebs = {}
for name, (p, h, t) in B.items():
    eb = arm_data.edit_bones.new(name)
    eb.head, eb.tail = h, t
    eb.roll = 0.0
    ebs[name] = eb
for name, (p, h, t) in B.items():
    if p:
        ebs[name].parent = ebs[p]
bpy.ops.object.mode_set(mode="OBJECT")

# ---- スキンウェイト: パーツのボーンを基本に、端の近くで隣のボーンへ滑らかに混ぜる ----
children = {}
for name, (p, h, t) in B.items():
    children.setdefault(p, []).append(name)


def seg_param(pt, h, t):
    d = t - h
    return max(0.0, min(1.0, (pt - h).dot(d) / d.length_squared))


BLEND = 0.28
for name in B:
    obj.vertex_groups.new(name=name)
for vi, bname in vert_bone:
    co = mesh.vertices[vi].co
    p, h, t = B[bname]
    s = seg_param(co, h, t)
    w = {bname: 1.0}
    if s < BLEND and p:
        a = 0.5 * (1.0 - s / BLEND)
        w = {bname: 1.0 - a, p: a}
    elif s > 1.0 - BLEND:
        kids = children.get(bname, [])
        if kids:
            kid = min(kids, key=lambda k: (co - B[k][1]).length)
            if (co - B[kid][1]).length < 0.16:
                a = 0.5 * ((s - (1.0 - BLEND)) / BLEND)
                w = {bname: 1.0 - a, kid: a}
    for bn, val in w.items():
        obj.vertex_groups[bn].add([vi], val, "REPLACE")

obj.parent = arm
mod = obj.modifiers.new("Armature", "ARMATURE")
mod.object = arm

# ---- 歩きのループ（1 秒 = 30 コマ + 閉じの 1 コマ）----
bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode="POSE")
pb = arm.pose.bones
for b in pb:
    b.rotation_mode = "XYZ"


def rad(d):
    return math.radians(d)


N = 30
for i in range(N + 1):
    ph = 2.0 * math.pi * (i / N)
    f = i
    for sd, off in (("L", 0.0), ("R", math.pi)):
        phi = ph + off
        s = math.sin(phi)  # +1 = 脚が最も前（-Y）
        # 前へ振る = X 軸まわりの回転。符号は preview の画像で確認する
        pb["thigh." + sd].rotation_euler = Euler((rad(-28.0 * s), 0, 0))
        knee = 42.0 * max(0.0, math.cos(phi))  # 振り出し（前へ戻す）の間だけ膝が曲がる
        pb["shin." + sd].rotation_euler = Euler((rad(knee), 0, 0))
        pb["foot." + sd].rotation_euler = Euler((rad(-0.25 * knee + 8.0 * s), 0, 0))
        pb["upperarm." + sd].rotation_euler = Euler((rad(22.0 * s), 0, 0))
        pb["forearm." + sd].rotation_euler = Euler((rad(-12.0 - 10.0 * max(0.0, s)), 0, 0))
    bob = -0.025 * (0.5 - 0.5 * math.cos(2.0 * ph))
    pb["hips"].location = Vector((0.015 * math.sin(ph), 0, bob))
    pb["hips"].rotation_euler = Euler((0, rad(3.0 * math.sin(ph)), rad(6.0 * math.sin(ph))))
    pb["spine"].rotation_euler = Euler((0, 0, rad(-4.0 * math.sin(ph))))
    pb["chest"].rotation_euler = Euler((0, 0, rad(-4.0 * math.sin(ph))))
    pb["head"].rotation_euler = Euler((0, 0, rad(3.0 * math.sin(ph))))
    for b in pb:
        b.keyframe_insert("rotation_euler", frame=f)
    pb["hips"].keyframe_insert("location", frame=f)

bpy.ops.object.mode_set(mode="OBJECT")
arm.animation_data.action.name = "Walk"

tris = sum(len(p.vertices) - 2 for p in mesh.polygons)
print("MANNEQUIN tris=%d verts=%d bones=%d" % (tris, len(mesh.vertices), len(B)))

# ---- 任意: 見た目の確認用に 2 コマをレンダリング ----
if preview_prefix:
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x, scene.render.resolution_y = 480, 720
    cam_d = bpy.data.cameras.new("cam")
    cam = bpy.data.objects.new("cam", cam_d)
    scene.collection.objects.link(cam)
    cam.location = (3.2, -3.2, 1.2)
    cam.rotation_euler = Euler((rad(84), 0, rad(45)))
    cam_d.lens = 70
    scene.camera = cam
    for frame in (0, 7, 15):
        scene.frame_set(frame)
        scene.render.filepath = "%s_f%02d.png" % (preview_prefix, frame)
        bpy.ops.render.render(write_still=True)

# ---- 書き出し（マネキンとリグだけ）----
bpy.ops.object.select_all(action="DESELECT")
obj.select_set(True)
arm.select_set(True)
bpy.context.view_layer.objects.active = arm
bpy.ops.export_scene.gltf(
    filepath=out_path,
    export_format="GLB",
    use_selection=True,
    export_apply=False,
    export_skins=True,
    export_animations=True,
    export_yup=True,
    export_image_format="NONE",
    export_cameras=False,
    export_lights=False,
)
print("MANNEQUIN wrote", out_path)
