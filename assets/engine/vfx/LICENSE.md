# assets/engine/vfx のライセンス

このフォルダの `*.dds` と `vfx_bake_manifest.json` は、`tools/vfx_bake/` の Python スクリプト(numpy のみ)が
**手続きで生成した自作のデータ**です。外部の素材・写真・他人のシミュレーション結果は一切含みません。

- ライセンス: **CC0 1.0(パブリックドメイン相当)**。出どころを示さずに使ってよい
- 再現: `python tools/vfx_bake/bake.py`(乱数は固定シード。同じ環境では同じ sha256 になる)
- 真正性: `vfx_bake_manifest.json` の sha256 を正とする(`python tools/vfx_bake/bake.py --verify`)
- 規約(軸、チャネル、MV の符号と範囲、6-way の向き)も同じ manifest の `conventions` に書いてある
