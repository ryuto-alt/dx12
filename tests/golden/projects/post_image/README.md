# post_image

ポスト画質の検証用ゴールデン（PI-01b / W1-NB4）。`tools/bench/temporal_metrics.mjs` と `energy_check.py` が使う 4 シーン。
プリミティブと手続きの空だけで組む（外部アセットなし）。既存シーン（golden_core ほか）には触れない。

| シーン | 何を見るか | 動かすもの（temporal_metrics が MCP で毎フレーム動かす） |
|---|---|---|
| taa_torture | 回る細い柵、高速パーティクル、発光の火花、サブピクセルの縞（TAA の残光・ちらつき・遮蔽解除） | `FenceHub` の回転、`GhostOrb` の出し入れ |
| dof_chart | 点光源の格子（ボケの形）と合焦面のテストチャート | なし（エネルギーの比は DoF の ON / OFF で撮る） |
| mb_wheel | 回る車輪とカメラのパン（モーションブラー） | `Wheel` の回転、カメラの横移動 |
| hdr_range | 空と日向（約 6e3〜8e3 nit）、室内 1 nit、Lamp 64 nit（物理ライティング単位・手動 EV100 15） | なし |

注: 仕様の「太陽円盤 1e9 nit・発光 1e5 nit」は未実現。発光強度の上限が 64（`kEmissiveIntensityMax`）で、手続きの空に太陽円盤が無いため。

- 静止画（golden.mjs の決定論スクショ）はシーンの初期状態をそのまま撮る。動きの検証は `tools/bench/temporal_metrics.mjs`。
- シーンの再生成: `node tests/golden/projects/post_image/gen_scenes.mjs`（出力は assets/scenes/*.json。手で JSON を書き換えない）。
- エンジンへ MCP で編集を入れると自動保存がシーン JSON に書き込みうるので、`temporal_metrics.mjs` はプロジェクトを一時フォルダへコピーしてから開く。
