# 拡張シェーディング（クリアコート / 異方性 / シーン / スペキュラ・IOR）の取り込み確認用 glb を作る。
# 実行（窓なし）: blender.exe -b -noaudio --factory-startup --python tools/blender/make_shading_test_glb.py
# 出力: tests/data/shading_ext.glb（正本は devgamePerfTestssetsmodelsshading_test）（球 7 個、材質名 = 球の名前）
import bpy, os, sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, "..", ".."))
out_dir = os.path.join(root, "game", "PerfTest", "assets", "models", "shading_test")
os.makedirs(out_dir, exist_ok=True)
out = os.path.join(out_dir, "shading_ext.glb")

bpy.ops.wm.read_factory_settings(use_empty=True)

# 名前, {入力名: 値}
SPECS = [
    ("Plain",     {}),
    ("Coat",      {"Coat Weight": 0.75, "Coat Roughness": 0.25}),
    ("Sheen",     {"Sheen Weight": 1.0, "Sheen Roughness": 0.4, "Sheen Tint": (0.2, 0.4, 0.8, 1.0)}),
    ("Aniso",     {"Anisotropic": 0.6, "Anisotropic Rotation": 0.25, "Metallic": 1.0, "Roughness": 0.3}),
    ("SpecLevel", {"Specular IOR Level": 0.25, "Specular Tint": (1.0, 0.5, 0.25, 1.0)}),
    ("Ior",       {"IOR": 1.33, "Specular IOR Level": 0.4}),   # ior は specular と同時のときだけ書き出される
    ("SpecZero",  {"Specular IOR Level": 0.0}),
]

for i, (name, vals) in enumerate(SPECS):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.5, location=(i * 1.2, 0, 0), segments=32, ring_count=16)
    ob = bpy.context.active_object
    ob.name = name
    bpy.ops.object.shade_smooth()
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    for k, v in vals.items():
        bsdf.inputs[k].default_value = v
    ob.data.materials.append(mat)

bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_materials='EXPORT')
print("WROTE", out)

# 書き出した JSON チャンクの材質部分を表示（取り込み値の記録用）
import json, struct
with open(out, "rb") as f:
    data = f.read()
clen, ctype = struct.unpack_from("<II", data, 12)
j = json.loads(data[20:20 + clen])
for m in j.get("materials", []):
    print("MAT", json.dumps(m, sort_keys=True))
print("EXTUSED", j.get("extensionsUsed"))
