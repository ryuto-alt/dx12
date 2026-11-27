# VFX(UnoFX)を置く・時刻を固定して撮る
> 焚き火・松明などの `.uvfx` は `dx12_vfx_place` で置き、`dx12_vfx_seek` で時刻を固定してから決定論で撮る。粒は静止画 1 枚では判断できない。

## 2 系統ある(混ぜない)
- **UnoFX(`.uvfx`)**: `dx12_vfx_place` / `dx12_vfx_seek` / `dx12_vfx_stats`。プリセットは `assets/engine/vfx2/presets/`。**新規はこちら**。
- **旧レシピ(複数レイヤーの放出器)**: `dx12_vfx_library` → `dx12_vfx_apply` → `dx12_vfx_preview`。UnoFX のプリセットはここの一覧に出ない。

## 置く(Editor モードのみ。Play 中は断られる → `dx12_stop`)
```
dx12_vfx_place {preset:"p_campfire", position:[0,0,0]}                       # プリセット名(engine/vfx2/presets/ の名前)
dx12_vfx_place {asset:"engine/vfx2/presets/p_torch.uvfx", name:"Torch_1", position:[2,1.5,0]}
dx12_vfx_place {preset:"p_torch", parentName:"TorchPost_1", position:[0,1.2,0]}  # 親(parent=entityId / parentName)の下に置く
```
- `preset` と `asset` は排他。どちらか 1 つ。`parent` と `parentName` も排他。
- 返り値の `entityId` を控える(seek の対象指定に使う)。置いた操作は Undo 1 件(`AI: vfx_place`)。
- 粒子は次のフレームから出る。置いた直後に撮らない。

## プリセット(`engine/vfx2/presets/`)
| 名前 | 内容 |
|---|---|
| `p_campfire` | 焚き火(炎・煙・火の粉) |
| `p_torch` | 松明の炎 |
| `p_embers_drift` | 漂う火の粉 |
| `p_sparks` | 火花 |
- 一覧は `assets/engine/vfx2/presets/*.uvfx`(アセットブラウザの `.uvfx` もダブルクリックで置ける)。名前が合わないとエラーの `fix` に置ける場所が出る。
- 自作は `.uvfx` を assets 配下へ置いて `asset:` で渡す。中身の誤りは読み込み時に場所つきで出る。

## 時刻を固定して撮る
```
dx12_vfx_seek {t:1.5}                          # 全 VFX を 1.5 秒の状態へ決定論で移して止める(同じ t は同じ絵)
dx12_vfx_seek {t:2.0, entity:<entityId>}       # 1 つだけ(name:"名前" でも指せる)
dx12_vfx_seek {t:0, resume:true}               # 頭から再生を続ける(止めない)
dx12_screenshot_final {deterministic:true, path:"shots/fire_t1p5.png"}
```
- `t` は 0〜60 秒。固定ステップ(1/60s)で再シミュレーションして止まる。**撮るのは seek の後に 1 フレーム以上回してから**。
- `resume:false`(既定)は止めたまま。撮り終えたら `resume:true` で戻す。
- 対象が無ければ先に `dx12_vfx_place`。

## 出ているかの確認
```
dx12_vfx_stats {}      # gpuReady / gpuFailed / poolUsed / poolCapacity / instances / vramBytes / lastError / list
```
- `gpuFailed:true` なら `lastError` を読む(シェーダ `.cso` が無いビルドなど)。VFX の予算は品質プリセット(`dx12_get_quality_preset`)の VFX の行に従う。
- 画が出ない → `dx12_vfx_stats` の `instances` と、`dx12_vfx_seek` 後の撮影を確認。ゴールデンは seek で時刻を固定し、DLSS を切って撮る。

## 次に読む
`dx12_guide {topic:"sequence"}`(カメラワークと一緒に VFX を動かす)/ `dx12_guide {topic:"lighting"}`(炎の周りの絵作り)
