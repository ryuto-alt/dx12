# forge - 小物を「その物だけのテクスチャ」に焼く工程

プロジェクトに依らない汎用部品。メッシュ(FBX/glb)+レシピ JSON から、物ごとの専用 UV に
スキャン素材 + 縁/くぼみ/上面/下のマスクで層を重ねた BaseColor / Normal / ORM(2K〜4K)を焼き、glb にする。
タイルの繰り返しとまだら汚れの代わりに、マスクに沿った摩耗・垢・埃・錆の垂れを出すのが目的。

## 使い方

```
python fetch_cc0.py                  # ambientCG(CC0)の 2K を %LOCALAPPDATA%\UnoEngine\cc0\ へ(1 回だけ。約 1GB)
blender -b -noaudio --python forge_bake.py -- --in mesh.fbx --recipe recipe.json --out out.glb
        [--res auto|1024|2048|4096] [--preview preview.png] [--front +x|-x|+y|-y] [--density 1024]
```

- Blender 5.2、Cycles GPU(OptiX → CUDA → CPU の順)。窓は出ない。重い処理なので BelowNormal で起動すること
  (ゲーム側の `forge_props.py` はそうしている)。
- `--res auto`: 焼く面の表面積から 1024px/m 相当の解像度(1024〜4096 の 2 の冪)。上限で密度が落ちる(焼却機は約 440px/m)。
- 出力: `out.glb`(焼いた分は 1 材質に統合、UV は焼き用 1 枚だけ、頂点カラーなし、factor は全部 1)と
  `out.glb.forge.json`(所要時間・解像度・三角形数・テクセル密度・各段階の秒)。
  作業用の PNG(マスク含む)は `<out の隣>/_forge_<名前>/`。
- glb の材質は**全部**に ORM(R=AO G=粗さ B=金属、occlusionTexture と metallicRoughnessTexture に同じ画像)を持たせる。
  焼かない材質(gfx・紙)には 4x4 の定数 ORM(粗さ 0.75、紙/ラベルは 0.85)。

## レシピ(recipe.json)

```json
{ "slots": {
  "paint_green": { "kind": "bake", "tex": "Metal032", "tile": 0.83, "tint": [0.1,0.17,0.13],
                  "flat": 0.97, "rflat": 0.5, "rough": 1.0, "metal": 0.0,
                  "wear": 0.9, "wearcol": [0.42,0.4,0.38], "wearmetal": 1.0,
                  "dirt": 0.6, "nstr": 0.04, "heat": 1.0, "rust": 0.6, "iron": true,
                  "packed_dir": "C:/.../textures" },
  "gfx_label":   { "kind": "image", "image": "C:/.../label.png", "alpha": "opaque|mask", "rough_const": 0.85 } } }
```

キーは Blender の材質スロット名(`.001` は外して引く)。レシピに無いスロットは既定値で焼く。
tex = ambientCG の id(キャッシュの 2K を使う。無ければ `packed_dir` の `<id>_{C,N,ORM}.png`)。tile = 1 タイルが何 m か(UV0 はメートル単位の箱投影)。
tint は乗算色、flat は 0.5 灰への寄せ、rflat はそのときの粗さ。wear は塗装剥げ(0 なら剥げ層なし)、dirt は垢・埃・下の汚れ、
heat は頂点色 G の熱焼け、rust は頂点色 B 等の錆の垂れ(`iron:true` のスロットだけ)。

頂点カラー `Col`: R=縁の摩耗 G=熱焼け B=垢・錆 のマスク。

## 層の順(下から)

下地(スキャン 2 回サンプル混ぜ)→ 粗さのむら → 縁のハイライト → 縁の塗装剥げ(縁 x 頂点色R x 崩し > 閾値、剥げの縁に暗い細線)→
くぼみ・下・大域 AO の垢 → 上面の埃 → 錆の垂れ(縦に伸ばしたノイズ x マスク)→ 熱焼け。
法線は スキャン法線(nstr)+ Bevel ノードの丸め(Cycles NORMAL 焼き、タンジェント空間 OpenGL = glTF)。
崩しのノイズは必ずマスクに掛けて出す(全面に均一なまだらにしない)。

## 罠

- Smart UV Project は曲面・ボルトを大量の小島にする。焼却機は約 24 万三角形で、表面積の大半が目に触れない面(内部・ボルト)。
  隠れ面の島を縮める可視性の重み付けは未実装(入れれば密度が上がる)。
- エッジ検出は Bevel ノードの法線差。メッシュに最初から面取りがあると弱く出る。半径は物の大きさで 3〜8mm。
- 凹角の誤検出を AO で外している。AO は `only_local`(同じ物の中だけ)。
- 材質を 1 枚に統合するので、`ember` / `lamp_red` / `tube` など**発光する材質も BaseColor に焼かれ発光は失われる**。
  発光を残したい物は recipe で `kind:"image"` に逃がすか、後で別材質にすること。
- glb は Y-up の標準(Blender の (x,y,z) → glTF の (x,z,-y))。FBX の `leftHanded` 扱いとは別経路なので、エンジンでの向き・鏡写しは要確認。
- 空(SOCKET_*)は glb にノードとして残る。
- 4096 の PNG 3 枚で glb が 20〜70MB になる。
- 床とライトはプレビュー専用で、書き出しの後に置く(glb には入らない)。
- アニメ(Idle/Run)の書き出しは未対応。口を作るなら: レシピに `"anim": {"Run": [{"object": "部品名" | "material": "スロット名", "rot": [..], "freq": ..}]}` を持たせ、
  部品ごとに分けたオブジェクトへ Action(回転・振動のキーフレーム)を付けて `export_animations` で書く。ただし現状は 1 メッシュに join して焼くので、
  動く部品だけ join から外して別プリミティブ(別ノード)にする変更が要る。
