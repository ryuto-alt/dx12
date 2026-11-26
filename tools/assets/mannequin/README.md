# マネキンと歩きのループ(NB14)

SC-1「校正の庭」の歩く人形と、LS-A のスキンドの影の縁に使う、自作(CC0)のスキンドのマネキン。
外部の素材は使わない。出力は `assets/engine/characters/mannequin/mannequin.glb`(約 130KB)。

## 作り直す

Blender(5.x で確認)をバックグラウンドで動かす。窓は出ない。

```
blender --background --factory-startup --python tools/assets/mannequin/build_mannequin.py -- assets/engine/characters/mannequin/mannequin.glb [<プレビュー PNG の接頭辞>]
```

接頭辞を渡すと Workbench で 3 コマ(0、7、15)を `<接頭辞>_fNN.png` に書く(見た目の確認用)。

## 仕様

- ボーン 19: hips、spine、chest、neck、head、(upperarm、forearm、hand、thigh、shin、foot、toe) の左右 `.L` / `.R`
- 三角形 3,724(上限 5,000)、無地のマテリアル 1 つ、テクスチャなし。スキンウェイトはパーツ基準で、関節の端 28% を隣のボーンへ混ぜる(最大 2 影響)
- アニメーション `Walk`: 1.0 秒、30fps、31 キー(0〜30 コマ。最後が最初と同じ。ループ)。その場で歩く(ルートモーションなし)
- 座標: glTF の Y 上、正面 +Z、足元 y=0、身長 約 1.8m

## 使う(MCP)

`dx12_import_asset` で `assets/` へ取り込み、`dx12_create_entity` などで `MeshRenderer`(`modelPath`)と Animator に割り当てる。
