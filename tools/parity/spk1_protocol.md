# SPK-1 計測手順: ReSTIR DI / SHaRC の計測スパイク

W2 C の末尾で行う計測スパイク(EXEC §3 の SPK-1)の準備。この文書は手順と基準だけで、実装は含まない。
採用の判断は下の「採用の基準」で機械的に行い、結果はオーナーへ報告する。

## 0. ライセンスと実装方針(最初に確認)

- **NVIDIA の RTX SDK(RTXDI / SHaRC / NRD 等)のソースも、そこから写したコードも、リポジトリに入れない。**
  ReSTIR DI(Bitterli ら 2020)と SHaRC の公開された論文・講演の記述だけを元に、**clean-room(自作)の試作**を書く。
  SDK のサンプルや HLSL を横に開いて写さない。NVIDIA の資料は「アルゴリズムの理解」にだけ使う。
- 試作は `FeatureRegistry` の 1 機能として入れ、**既定 OFF**。ビルドに SDK を要求しない(サブモジュール・FetchContent を足さない)。
- 外部のバイナリ・資産は使わない。計測用シーンは自作(`gen/manylights_scenes.py`)で、CC0 相当。
- 不採用のときは試作ごと取り除ける形にしておく(W8+ へ送る)。

## 1. 計測シーン

| id | 灯数 | 内訳 | 1 灯の強さ / 半径 |
|---|---|---|---|
| `ML-64`(`scenes/manylights/ml_64.json`) | 64 | 回廊 16 + 中庭 48 | 7.0 / 中庭 6.0m・回廊 5.0m |
| `ML-256`(`scenes/manylights/ml_256.json`) | 256 | 回廊 64 + 中庭 192 | 2.2 / 中庭 4.5m・回廊 3.5m |

- 形状は共通: 24x24m・高さ 6m の屋根付き中庭(柱 8・低い仕切り壁 3・植木鉢・中央の台・金属球)と、南壁の開口から伸びる幅 4m・長さ 30m の回廊(柱の出っ張り 3 組)。
- 太陽は 0。全灯 `castShadows=false`(ラスタの点光源の影は 16 灯までなので、多灯は影なしの総当たりが現行)。
- カメラ 4 つ: `court_wide` / `court_corner` / `threshold`(中庭から回廊の入口)/ `corridor_deep`。
- 灯は固定シード(20261127)から決める。再生成しても同じファイルになる: `python tools/parity/gen/manylights_scenes.py`。
  灯の数・強さを変えたいときは `gen/manylights_scenes.py` の `VARIANTS` を直して再生成し、テスト(`tests/test_manylights_scenes.py`)を通す。
- 灯の数はエンジンの上限(合計 1024、クラスタ 1 マス 128)の内側。テストが 1 灯の範囲に入る灯の数を検査する。

## 2. 撮り方(PT の参照とラスタの基準線)

窓は出さない・実マウスを使わない: エンジンは `tools/engine_instance.ps1` が背景(headless)で起動し、終わりに必ず止める。
ビルドしないセッションは、他のレーン / 本流の `build\release` を `--build-dir` で指す。

```powershell
# tools/parity を作業ディレクトリにして実行する
$env:PYTHONUTF8 = '1'
# PT 1024spp + 現行ラスタ(legacy)を 2 シーン x 4 カメラ撮る。出力は <repo>/.dx12/parity/spk1/<日時>/(git 管理外)
.\.venv\Scripts\python.exe -m parity.spk1_capture --build-dir <build\release のあるフォルダ> --port <自分のレーンのポート> --spp 1024
# 1 シーンだけ・軽い確認(2 シーン 4 カメラで約 2 分)
.\.venv\Scripts\python.exe -m parity.spk1_capture --build-dir <...> --port <...> --scenes ml_64 --spp 128 --out <dir>
```

- 出力: `<out>/<シーン>/<カメラ>/pt.pfm`(正解)・`raster_<構成>.pfm`(基準線)と、見るための `pt.png` / `raster_*.png`(PT の幾何平均で露出を合わせた ACES 表示)、`capture.json`。
- PT は `render_reference`(1024spp・8 バウンス・seed 1)。PT のノイズ床を知りたいときは `--seed 2` でもう 1 回撮って PT 同士の FLIP を取る(ノイズ床。ReSTIR 側の FLIP はこれより良くはならない)。
- ラスタは `--capture-mode det`(決定論 deterministic・settleFrames 240)が既定。DLSS OFF・TAA OFF(`legacy` 構成が `set_taa` を切る)。
- **試作を入れた後**は、`gi_eval.CONFIGS` に `restir_di` / `sharc` 相当の構成(`set_*` の MCP method)を足し、`--configs legacy,restir_di` で同じ手順で撮る。
- GPU を他と取り合わない: 計測窓のロック(`%TEMP%\uno-gpu-window.lock`)を取り、計測前に VRAM の空きを確認する。ゲーム中・ユーザーが作業中は PT を回さない(離席中に回す)。

## 3. 計測する値

内部解像度は **1280x720**(出力 1920x1080 相当の DLSS Quality)。

| 指標 | 取り方 |
|---|---|
| GPU 時間(ms) | 試作のパスの合計(ReSTIR DI: 候補生成 + 時間再利用 + 空間再利用 + シェーディング。SHaRC: 更新 + 参照)。`perf_gate` の ABAB(A/A のノイズ込み)で、試作 OFF との差を取る。クロックを記録する |
| VRAM(MB) | 試作の追加分(リザーバ・ハッシュグリッド等)。`D3D12MA` の統計で OFF との差を取る |
| FLIP | 構成ごとに、PT(1024spp)との LDR-FLIP の平均(`gi_eval` と同じ計算: `parity.gi_metrics` / `compute_flip`)。カメラ 4 つの平均とワーストを出す |
| 時間安定 | 静止 8 フレームの差(`tools/bench/temporal_metrics.mjs`)。ちらつき・ゴーストが出ていないか |

比較する基準線は **現行**(`legacy` = 影なし総当たり)と、LS-08 相当(アトラス + 総当たり)が入っていればそれ。

## 4. 採用の基準(EXEC §3 の SPK-1)

- **ReSTIR DI**: `ML-64` と `ML-256` の両方で、
  - 内部 1280x720 での合計が **0.6ms 以下**
  - VRAM 追加が **32MB 以下**
  - PT との FLIP が現行(LS-08 相当のアトラス + 総当たり)より **20% 以上良い**
- **SHaRC**: 更新と参照の合計が **0.5ms 以下**、VRAM **64MB 以下**。他のレンダリングの予算の行(§3)を削らずに収まること。
- 満たさなければ **W8+ へ送る**。結果(表と PNG)はオーナーへ。数値は予算ファイル(`testing/budgets/*.json`)に書き、スクリプトにも文書にも別の数字を持たない。

## 5. 手順のチェックリスト

1. `python tools/parity/gen/manylights_scenes.py` で 2 シーンを再生成し、差分が無いこと。
2. `pytest tools/parity/tests/test_manylights_scenes.py` が緑。
3. `parity.spk1_capture --spp 128` でシーンが開く(エラー無し・露出が極端でない)ことを確かめる。
4. 本番の PT(1024spp x 2 シーン x 4 カメラ)は離席中に回す。
5. 試作が入ったら §3 の表を埋めて §4 で判定。NVIDIA のソースが混ざっていないことをレビューで確かめる(§0)。
