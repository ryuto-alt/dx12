# assets/engine/characters/mannequin のライセンス

`mannequin.glb` は、`tools/assets/mannequin/build_mannequin.py`(Blender 5.x の Python)が
**手続きで生成した自作のデータ**です。外部の素材・スキャン・他人のモデルやモーションキャプチャは一切含みません。

- ライセンス: **CC0 1.0(パブリックドメイン相当)**。出どころを示さずに使ってよい
- 内容: 19 ボーンのスキンド メッシュ(楕円体 19 個、三角形 3,724、頂点 1,900、無地のマテリアル、テクスチャなし)と、
  1 秒(30fps、31 キー、0 コマ目と 30 コマ目が同じ)の歩きのループ `Walk`。身長 1.8m、足元が原点
- 再現: `README.md`(`tools/assets/mannequin/`)のコマンドで作り直せる(生成物はビット単位では一致しない。三角形の並びが実行ごとに変わりうる)
