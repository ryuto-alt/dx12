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
- アニメーション `Walk`: 1.0 秒、30fps、31 キー(0〜30 コマ。最後が最初と同じ。ループ)。その場で歩く(ルートモーションなし。骨盤の上下は各コマで最下点が y=0 になるよう計算し、足は常に床に触れる。CPU スキニングで全 31 コマの最下点 |y| < 1e-6 を確認)
- 座標: glTF の Y 上、正面 +Z、足元 y=0、身長 約 1.8m

## 使う(MCP)

確認済みの手順: `import_asset`(`assets/` へ取り込み。作り直したら `overwrite:true`)→ `spawn_model` → `play_anim`(clip 名 `Walk`、ループ。`asset_info` の durationSec は 1.0)。
マテリアルは片面(`doubleSided` なし)。生成はビット単位で一致しない(三角形の並びが実行ごとに変わりうる)。

## エンジンでの接地の確認(FU-E-34)

作り直したら、エンジン(--headless・lane-d は port 8854)で読み込んで足が床に着いているかを確かめる。窓は出ない・マウスに触らない。

```
pwsh -NoProfile -File tools/parity/mannequin_feet.ps1 --build-dir <build\release のあるフォルダ>
```

カメラを y=0・水平に置き(y=0 の点は奥行きに関係なく中心行に写る)、マネキンのいない参照との差分で最下行を取り、`Walk` の 30 コマ(Play 中・固定 dt=1/30)すべてで |浮き / めり込み| <= 2cm を合格とする。エディタモードはアニメの時間が止まるので Play で撮る。判定だけの再実行は `--from-dir`。
実測(2026-11-27、本流の build/release): 30 コマとも最下行が中心行と一致(差 0.0cm、1 画素 = 約 0.7cm)、clipTime は 0.18 から 1 周して 0.15 まで進んだ。
