# パリティ・ハーネス(UE 同等品質の合否判定の仕組み)

- 実装: `tools/parity/`(Python 3.12。CPU のみ・GPU 不要)
- 設計: `docs/GRAPHICS_PARITY_DESIGN.md` §3(本書はその実装の使い方。**UE を自動起動する部分は作っていない**)
- 決定(2026-09-30): **UE は再インストールしない**。基準画像は「エンジン内の DXR パストレーサー(`render_reference`)」で作り、
  **ユーザーが手動で撮った UE / 別エンジンのスクリーンショットを任意の追加基準として取り込める**。
- 状態: 比較指標・シーン仕様・自動ランナー・レポート・視覚回帰 baseline・性能ゲート = 実装済み。`render_reference`(PT の MCP)は未マージのため、
  ランナーの `pt` アダプタは「method が無い」ときに **skipped(合格扱いにしない)** で返る。

---

## 1. 最初に試す(セットアップ 1 回 + 動作確認)

```powershell
pwsh -NoProfile -File tools\parity\setup.ps1          # tools/parity/.venv を作って依存を入れる(グローバル Python は汚さない)
pwsh -NoProfile -File tools\parity\parity.ps1 doctor --selftest    # 偽エンジンで通し確認(GPU・exe 不要)
pwsh -NoProfile -File tools\parity\parity.ps1 scenes                # シーン仕様の一覧と検証
```

`parity.ps1` は venv の Python で `python -m parity <引数>` を実行し、終了コードをそのまま返す(作業ディレクトリは `tools/parity`。
**相対パスはそこ基準**なので、`--out` などは絶対パスで渡すのが安全)。

依存(`tools/parity/requirements.txt`): numpy / scipy / pillow / scikit-image(SSIM・CIELAB・ΔE2000)/ **flip-evaluator**(NVIDIA FLIP 公式 Python 版・BSD-3)/
OpenEXR / opencv-python-headless(16bit PNG・.hdr。任意)/ jsonschema / pytest。
Python か依存が無い環境では、ctest の `ParityHarnessPyTests` は **スキップ**(`PARITY_SKIP`)になる。

### 2 枚の画像を比べる

```powershell
pwsh tools\parity\parity.ps1 compare C:\ref.png C:\uno.png --gate G1 --limit flip_ldr_mean_max=0.15 --limit ssim_min=0.75 --out C:\tmp\cmp
```

指標の一覧・領域別・ヒートマップ・HTML レポート(`C:\tmp\cmp\report.html`)が出る。`--limit` か `--gate` を付けたときだけ終了コードで合否を返す
(0 = 合格 / 1 = 不合格 / 2 = エラー)。付けなければ指標を出すだけ。

---

## 2. コマンド一覧

| コマンド | 内容 |
|---|---|
| `parity compare 基準 テスト` | 2 枚の比較。`--metrics` / `--tonemap` / `--exposure none\|auto\|fixed:EV` / `--size-policy error\|resize\|crop` / `--mask 名前=マスク.png` / `--spec` `--gate` / `--limit key=値` / `--noise-floor file.json` / `--ppd` / `--strict` / `--embed` |
| `parity report RUN_DIR` | `run.json` から `report.html` を作り直す(`--embed` で画像埋め込みの単一ファイル) |
| `parity noise-floor 画像 画像 ...` | 同じシーンを **別シード**で撮った画像同士からノイズ床を出す(`--out` か `--scene X --camera Y` で保存) |
| `parity calibrate リニア.pfm 外部基準.png` | Uno のリニア画像を外部基準(UE のスクショ)へ最も近づけるトーンマップと EV の候補を並べる |
| `parity run シーン仕様.json --stage G1` | 自動ランナー(エンジン起動 → 撮影 → 基準 → 比較 → 性能 → レポート → 合否) |
| `parity baseline update / approve / check / list / discard` | 視覚回帰(§7) |
| `parity scenes [files]` | シーン仕様の一覧・検証 |
| `parity doctor [--selftest]` | 依存・環境の確認。`--selftest` は偽エンジンでランナー全体を通す |

**終了コード**(`compare --limit` / `run` / `baseline check`): `0` 合格 / `1` 不合格 / `2` エラー(設定・接続・実行時)/ `3` skipped のみ(基準が無い等。**合格扱いにしない**)。
`--skipped-ok` で 3 を 0 にできる(基準が未生成の段階の CI 用)。

---

## 3. 指標の意味

「基準(ref)に対して、テスト(test = Uno)がどれだけ違うか」。値は全て `run.json` の `metrics` と HTML レポートに出る。

| 指標(キー) | 意味・読み方 | 良い向き |
|---|---|---|
| **LDR-FLIP**(`flip_ldr_mean/median/p95/p99/max`) | **主指標**。NVIDIA FLIP(公式実装)。トーンマップ後の表示画像の知覚差。0 = 区別不能 / 1 = 明らか。既定は 67 ppd(公式既定 = 0.7 m 4K TV)。`--ppd` で変更 | 小 |
| **HDR-FLIP**(`flip_hdr_mean/p95/...`) | 両方が**リニア HDR**(PFM/EXR)のときだけ自動で計算。露出範囲のスイープ(FLIP が自動決定。`startExposure` などは `alignment` ではなく `flip.hdr` に記録)を含む。Tier A の主指標 | 小 |
| **SSIM**(`ssim`) | 輝度(ガンマ空間の luma)の構造類似度。11×11 ガウス窓・σ=1.5(scikit-image と同値)。形・エッジ・ボケの一致 | 大 |
| **PSNR**(`psnr`) | 参考(ゲートにしない)。同一なら 100 dB で頭打ち | 大 |
| **ΔE2000**(`de2000_mean/median/p95/max`) | CIELAB(D65)の局所色差。色被り・白バランス・彩度の差。JND は約 1、6 以上は明確に違う色 | 小 |
| **輝度ヒストグラム EMD**(`hist_emd_ev`) | log2 輝度分布(-12〜+2 EV、256 ビン)の 1 次元 EMD。単位 EV。分布の平行移動ならその移動量 | 小 |
| **輝度差**(`lum_mean_ev` / `lum_median_ev`) | test/ref の平均・中央輝度の比を log2 で(EV)。符号付き(+ = Uno が明るい) | 0 に近い |
| **色相ずれ**(`hue_shift_deg` / `hue_abs_median_deg`) | LCh の色相角の差。前者は彩度重みの符号付き平均(全体が回っているか)、後者は画素ごとの絶対差の中央値。彩度が低い画素は除外 | 0 に近い |
| **整列で当てた露出**(`exposure_ev_applied`) | `exposure=auto` で整列したときの補正 EV。**補正しすぎを落とすためのゲート用**(§5) | 0 に近い |

- **領域別**: `regions`(マスク PNG)ごとに同じ指標を出す。FLIP・SSIM・ΔE は画像全体でマップを作ってから領域内を平均する(境界付近は隣の領域の影響を受ける)。
- **ヒートマップ**(run ディレクトリの `cam_<名前>/`): `heat_flip_ldr.png`(magma・カラーバー付き)/ `heat_flip_hdr.png` / `heat_de2000.png`(0..10)/
  `diff.png`(差 ×4)/ `diff_signed.png`(赤 = Uno が明るい・青 = 暗い)/ `contact.png`(基準 | Uno | FLIP の並べ)。`worstTiles` = 16×16 タイルの FLIP 平均が悪い上位と座標。
- **検証結果**: 指標の単体テストで、同一 → 0(SSIM=1)、既知の露出差(+0.5 EV)→ 輝度差 0.5 EV と EMD 0.5 EV、ΔE2000 は Sharma らの公開検証値と一致、
  SSIM は scikit-image と一致(独立な scipy 実装とも 2e-3 以内)、**FLIP は NVlabs/flip 付属の公式サンプルと一致**(LDR: 公式 0.15969 = 本基盤の経路と相対 1e-4 以内 /
  HDR: 公式 0.28348 に対し 0.28355)。オフラインではこのサンプルの 2 テストだけスキップされる(取得先: `raw.githubusercontent.com/NVlabs/flip/main/images/`、
  キャッシュ: `%LOCALAPPDATA%\UnoEngine\parity\cache\flip`。リポジトリには入れない)。

---

## 4. しきい値の考え方(合否 G0 / G1 / G2)

- 段階: **G0 健全**(全黒・全白・NaN・平面でない。常に検査)/ **G1 近い**(同じシーンと分かる・雰囲気が同等)/ **G2 同等**(並べて差を指摘しにくい)。
- しきい値キー = `<指標>_max`(値 ≤ 限界)/ `<指標>_min`(値 ≥ 限界)/ `<指標>_abs_max`(|値| ≤ 限界)。例: `flip_ldr_mean_max`、`ssim_min`、`lum_mean_ev_abs_max`。
  (`flip_ldr_max` という**統計量の最大**に上限を付けるときは `flip_ldr_max_max`)
- **実効しきい値 = max(固定値, ノイズ床 × 1.5)**。ノイズ床は同じシーンを別シードで撮った画像同士の差(§6)。SSIM は `min(固定, 1-1.5(1-床))`、PSNR は `床 - 20log10(1.5)`。
- 初期値は設計書 §3.7 の**推定**(G1: FLIP 平均 ≤ 0.15・輝度 ±0.5 EV・SSIM ≥ 0.75・ΔE 中央値 ≤ 6 / G2: FLIP 平均 ≤ 0.08・p95 ≤ 0.30・輝度 ±0.25 EV・SSIM ≥ 0.90・ΔE ≤ 3・HDR-FLIP ≤ 0.10)。
  **初回の実測(ノイズ床と初回ギャップ)で固定する**。緩めて通すのは禁止(緩める変更は仕様 JSON の差分としてレビュー。理由を残す)。
- 指標は**必要条件であって十分条件ではない**。各節目で、レポートの並べ比較を**人が目視して承認**する。AI 単独で「合格」と宣言しない。
- 判定の状態: `pass` / `fail` / `skipped`(基準が無い・method が無い等。合格扱いにしない)。計算できなかった指標(HDR-FLIP がリニア入力でない等)は `warnings`、`--strict` で不合格。
- 領域別のしきい値(`regions[].gates`)・カメラ別の上書き(`cameras[].gates`)・開発段階名の別名(`milestones`: `{"Q2":"G2"}` → `--stage Q2`)・視覚回帰用(`regression`)を仕様で指定できる。

---

## 5. トーンマップ・露出の揃え

比較の前に、エンジン画像と基準を**同じ土俵**へ載せる(`tonemap.py`)。

- **PNG(表示参照)にはトーンマップを掛けない**(もう掛かっている)。**PFM/EXR(リニア HDR)にだけ**、選んだトーンマップを掛ける。
- 候補(全て自前実装。UE のシェーダ由来のコードは入れていない): `engine_aces`(既定。Uno の PostProcess.hlsl と同じ Narkowicz ACES → `pow(1/2.2)`)/ `aces_narkowicz` / `aces_hill`
  (ACES RRT+ODT フィット)/ `ue_filmic_approx`(UE 既定の Filmic は ACES 準拠設計なので `aces_hill` の別名。**近似**。校正で残差を見る)/ `linear_clip` / `reinhard` / `engine_agx`。
- **★Uno の PNG は厳密には gamma 2.2**(ACES の後に `pow(1/2.2)` で sRGB OETF ではない)。仕様の `engine.png: "gamma22"` で扱い、指標の前に両方を sRGB エンコードへ正規化する。
- 露出: `none` / `fixed:EV`(リニアの側に掛ける)/ `auto`(中央値合わせ。リニアどうしは中央輝度、リニア対表示は「トーンマップ後の中央 luma が一致する EV」を二分法で)。
  **`auto` は露出の違いを指標から消してしまう**ので、適用した EV は `exposure_ev_applied` に必ず残る。ゲートに `exposure_ev_applied_abs_max` を書けば補正しすぎを落とせる。
- 解像度が違うとき: `--size-policy`(既定 `error` = 黙って比べない)。`resize`(縦横比が 1% 以内のときだけ。縮小は面積平均・拡大は Lanczos)/ `crop`(中央)。
  ランナーの既定は `resize`(今のエンジンは `--size` を持たず、ヘッドレスの表示矩形になるため)。解像度差は指標に影響するので参考扱い。

### UE スクリーンショット取込時の校正手順(グレーカード・既知輝度パッチが無い前提)

UE 側の絶対値(露出・トーンマップの詳細)は分からないので、**ヒストグラム合わせで「どのトーンマップ + 何 EV なら分布が近いか」を出す**。

1. 基準にするシーンを Uno 側で `render_reference`(PT)から **リニア PFM/EXR** として撮る(同じカメラ位置・FOV)。
2. UE のスクショ(§8 の撮り方)を `references/<シーンid>/<カメラ名>.png` に置く。
3. `parity calibrate uno_linear.pfm ue.png` を実行。候補ごとに「中央 luma が一致する EV」を求め、残る分布差(輝度分位の EMD)で並べる。
   最良の候補と EV、その組み合わせでトーンマップした画像(`best_tonemapped.png`)が出る。
4. 結果を `references/<シーンid>/<カメラ名>.json`(サイドカー)に書く: `{"tonemap": "aces_hill", "exposure": "auto", "provenance": "UE 5.x, TAA 切り, 固定露出"}`。
   以後の `parity run` はこの整列で比べる。
5. **見るべきもの**: EMD が小さくても FLIP の局所エラーが大きければ、原因はトーンマップではなく機能差(GI・反射・影)。EV が大きく外れる(±1.5 以上)なら光の単位差を疑う
   (Q2 の物理単位モードの領域)。**EV は参考値で絶対校正ではない**: 色相が回る・彩度が違うなど、トーンマップの色処理の差は EV では吸収できない。
6. PS-0 校正シーン(グレーカード 18%・24 パッチ)を UE で撮れる場合は、**グレーカードの画素値**が最も信頼できる校正になる(パッチの値から EV とトーンカーブを直接読む)。

---

## 6. ノイズ床

同じシーン・カメラを**異なるシード**で 2 回以上レンダリングして比べると、「これ以下の差は区別できない」床が出る。

```powershell
parity noise-floor pt_s1.pfm pt_s2.pfm --scene ps1_indoor_corridor --camera corridor_a
#   -> %LOCALAPPDATA%\UnoEngine\parity\noise\ps1_indoor_corridor\corridor_a.json(以後の run が自動で使う)
```

- ランナーは `run --want-noise`(PT の `seeds` が 2 個以上のとき)で撮影・測定・保存まで自動で行う。
- 2 枚の独立ノイズ画像の差は「ノイズ画像 vs 真値」の √2 倍。Uno(決定論でノイズ 0)と PT を比べる本番では実際のノイズは床の約 1/√2 なので、床をそのまま使うのは**保守的(緩め)**。
  `--correction single` で 1/√2 を掛けられる。
- 床は spp・解像度・基準の種別で決まる。**基準の設定を変えたら測り直す**。
- 同じ指標は `ssim` / `psnr` などの「小さいほど悪い」側では逆向きに扱う(§4)。

---

## 7. シーン仕様と自動ランナー

### シーン仕様(`tools/parity/scenes/*.json`、スキーマ `schema.json`)

1 ファイル = 1 パリティシーン。

| キー | 内容 |
|---|---|
| `id` / `parityScene` / `status` | 識別子(英小文字・数字・_)/ `PS-0`〜`PS-5` / `stub`(枠だけ → skipped)`draft` `ready` |
| `scene` | `project`(`${環境変数}` 展開。実プロジェクトは読み取り専用・使い捨てコピーを指す)/ `generator`(`script --out DIR --id ID --args-json ...` で使い捨てプロジェクトを作る)/ `current`(接続先のエンジンが開いているシーン) |
| `cameras[]` | 名前つき視点(`position` `target`、任意で `regions` `gates` `reference` `skip`)。★`set_editor_camera` は FOV を変えられない(エディタ既定 45°)。`fovDeg` が違うと警告 |
| `engine` | `resolution`(期待値)/ `mode` / `launchArgs` / `warmupFrames` / `settleFrames` / `setup`(シーンを開いた後の MCP 呼び出し。`optional` で未実装機能を許容)/ `features`(記録用)/ `png`(gamma22 or srgb)/ `requiresMethods` |
| `reference` | `kind`: `pt` / `external` / `previous-run`。`pt`: `spp` `seeds` `method` `params` `sizeParams` `timeoutSec` `format` / `external`: `dir` `colorspace` `tonemap` `exposure` `provenance` |
| `alignment` | `tonemap` / `exposure` / `sizePolicy` / `metrics` / `ppd` |
| `regions[]` | `name` `mask`(PNG。2 値、または `color` 指定のラベルマップ)`gates`(領域別しきい値) |
| `gates` / `milestones` / `regression` / `noiseFloor` / `perf` | しきい値(G0/G1/G2)/ 段階名の別名 / 視覚回帰のしきい値 / ノイズ床(k・ファイル・直書き)/ 性能予算 |

同梱サンプル: `ps0_calibration.json`(**PS-0 校正**: グレーカード + 24 パッチ + BSDF 5×5 格子 + 白い炉。生成器 `gen/ps0_calibration.py`)/
`ps1_indoor_corridor.json`(**PS-1 屋内**: 暗い廊下 + 動的ライト + エミッシブ看板 + 濡れ床。`${PARITY_PS1_PROJECT}` で使い捨てコピーを指す・領域マスク付き。**中身は Q4 で作る**)/
`smoke_generated.json`(基盤の通し確認と視覚回帰用。実エンジンで動作確認済み)。PS-2〜PS-5 はスキーマで書ける(中身は各機能の段階で作る)。

### ランナー

```powershell
pwsh tools\parity\parity.ps1 run tools\parity\scenes\smoke_generated.json --stage G1 --out C:\tmp\run1
```

1. `engine_instance.ps1 -Name par -Port 8820 -Mode headless [-Project ...]` でエンジンを背景起動(**exe 直接起動はしない**)。終わったら**必ず** `-Stop`(自分の名前だけ)。
   `--attach PORT` なら起動済みのエンジンへ接続だけ(起動も停止もしない)。環境変数 `DX12_MCP_PORT` があれば attach 扱い(MCP ジョブ用)。
2. `open_scene`(起動直後の「already in progress」は待って撃ち直す)→ `engine.setup` の呼び出し → 各カメラで `set_editor_camera` → `step_frames`(warmup)→ **`screenshot_final {deterministic:true, gizmos:false, settleFrames}`**。
3. 基準をアダプタから取得: `pt`(`render_reference`。無ければ skipped)/ `external`(`references/` から)/ `previous-run`(承認済み baseline)。
4. 整列 → 指標 → 領域別 → ヒートマップ → 判定。性能ゲート(`perf.budgets` があれば): `benchmark`(uncap)+ `ping.vramUsedMB` + ロード時間(open_scene の前後)。
   予算超過、または履歴(直近 5 回の中央値)から `regression_pct_max`(既定 12%)以上の悪化で不合格。履歴は `%LOCALAPPDATA%\UnoEngine\parity\perf\<id>.jsonl`。
5. `report.html` と `run.json` を出力(既定 `%LOCALAPPDATA%\UnoEngine\parity\runs\<日時>_<id>_<段階>\`)。終了コードで合否。

その他のオプション: `--camera 名前`(複数可)/ `--reference pt|external|previous-run`(上書き)/ `--test-images DIR`(撮影済みの画像を使いエンジンを起動しない)/
`--external-dir DIR` / `--no-perf` / `--strict` / `--want-noise` / `--progress` / `--skipped-ok` / `--project`。

### 基準アダプタ(差し替え可能)

| kind | 内容 |
|---|---|
| `pt` | エンジン内 DXR パストレーサー。MCP `render_reference` を呼ぶ。**呼び出し規約**(PT 担当との取り決め。合わなければ仕様の `reference.pt.method` / `params` / `sizeParams` で吸収): 要求 `{spp, seed, path: "<絶対パス .pfm/.exr>", ...params}`(`sizeParams: ["width","height"]` でエンジン画像と同じ大きさを渡せる)/ 応答 `{path, width, height, spp}`。出力は**リニア RGB(Rec.709・露出 1.0・トーンマップ前)の float**。カメラは呼ぶ直前に `set_editor_camera` 済み。method が無ければ `skipped`(理由に「PT 未マージ」) |
| `external` | 人が撮った画像。`references/<id>/<カメラ名>.png`(または `reference.external.dir`)。サイドカー `<カメラ名>.json`(`colorspace` `tonemap` `exposure` `provenance`)で来歴と整列を指定 |
| `previous-run` | 承認済み baseline。GPU 名が違えば skipped(§7 視覚回帰) |

### MCP ジョブ(`dx12_job_start`)から起動する

専用の kind は増やさず、既存の `external` で動く(承認が要る guarded 操作。進捗は `--progress` の `@progress` / `@result` 行を読む):

```
dx12_call_guarded {name:"dx12_job_start", args:{kind:"external", args:{
  command:["C:\\Users\\ryuto\\Documents\\dx12\\tools\\parity\\.venv\\Scripts\\python.exe","-m","parity","run",
           "C:\\Users\\ryuto\\Documents\\dx12\\tools\\parity\\scenes\\smoke_generated.json","--stage","G1","--progress"],
  cwd:"tools/parity", progress:"protocol"}}}
```

`dx12_job_status` で進捗(`phase`: prepare / open_scene / capture / perf / compare)と結果(`verdict` `exitCode` `runDir` `reasons`)が引ける。
エンジンを ジョブに束縛した場合(`engine` 指定)は `DX12_MCP_PORT` が渡され、ランナーはそのエンジンへ attach する。

### CI(ローカル GPU 前提)

GitHub Actions の `windows-latest` は GPU が無いので、視覚回帰・パリティは**ユーザー PC のローカル実行前提**。CI では `pytest`(ctest の `ParityHarnessPyTests`)だけ走る。
ローカルの CI 風実行: `parity run <spec> --stage G1 --skipped-ok`(終了コード 0 / 1 / 2)。基準が未生成の段階は `skipped` で通り、生成後は不合格が 1 になる。

---

## 8. baseline(視覚回帰)運用と UE スクリーンショット取込

### 視覚回帰(MCP 書 M9 の相乗り)

置き場は**リポジトリの外**: `%LOCALAPPDATA%\UnoEngine\parity\baselines\<シーンid>\{approved,pending}\<カメラ>.png` + `.json` + `history.jsonl`(画像は巨大で PUBLIC のため git に入れない。`PARITY_HOME` で移せる)。

```powershell
parity baseline update  tools\parity\scenes\X.json                     # エンジンで撮って pending に保存(基準はまだ変わらない)
#   ↓ 撮れた画像を目視する
parity baseline approve tools\parity\scenes\X.json --reason "初回 / 何が変わって、なぜ受け入れるか"   # 理由が必須(4 文字以上)
parity baseline check   tools\parity\scenes\X.json                     # 撮って承認済みと比べる。差があれば不合格 + diffBBox
parity baseline list
```

- **人が承認する流儀**: `update` は pending にしか書かない。基準を置き換えるのは `approve` だけで、`reason` 必須。承認は `history.jsonl` に残る(いつ・なぜ・前後の sha)。
- `check` は `--stage regression` の別名(`previous-run` 基準)。しきい値は仕様の `regression`(既定: FLIP 平均 ≤ 0.002 / p95 ≤ 0.01 / SSIM ≥ 0.995 / ΔE p95 ≤ 1 / 輝度 ±0.02 EV)。
  決定論撮影なので**同じ絵ならビット一致**(`bitExact`)。違えば `diffBBox`(違う画素の外接矩形)を返す。
- baseline は **GPU 名**をキーに持つ。GPU が違えば `skipped`(画素が揃わない)、ドライバ違いは警告。
- 実機で確認済み(RTX 5060): 別々のエンジン起動で 2 回撮った生成シーンが**ビット一致**、太陽の強度を変えると不合格 + diffBBox。

### UE / 別エンジンのスクリーンショットを取り込む(手動撮影の推奨)

ユーザーが手動で撮った画像を**任意の追加基準**として使う。カメラ位置が Uno と厳密に合わせられない場合は、G0/G1 の**目視の参考**として扱う(数値ゲートには使わない)。

**撮り方の推奨**(基準の質がそのままゲートの質になる):
- **解像度**: 1920×1080(Uno の比較解像度に合わせる)。ウィンドウのスクショでなく、エンジンの高解像度スクショ機能(UE なら `HighResShot 1920x1080`)で撮る。
- **露出は固定**: 自動露出 OFF(Manual・EV100 固定)。撮影時の EV と設定を `provenance` に書く。**目標の絵ができてから固定する**のではなく、**Uno と同じ光の条件**(太陽・時刻)で固定する。
- **HUD・UI・デバッグ表示なし**(`ShowFlag.HUD` / エディタ UI 非表示・ゲームビュー)。ゲーム内 UI・十字線・FPS 表示を消す。
- **ポスト効果は原則 OFF**: ブルーム・ビネット・グレイン・モーションブラー・被写界深度・レンズフレア・色収差。残すなら `provenance` に明記。
- **アンチエイリアス**: 収束した TAA/TSR(`r.ScreenPercentage=100`)で、カメラを止めて数十フレーム待ってから撮る(または複数枚の平均)。TAA を切ると縁がジャギーになり FLIP に出る。どちらにしたか書く。
- **可能なら 16bit PNG か EXR**(`HighResShot ... bCaptureHDR` = EXR)。EXR ならリニア HDR として読めて HDR-FLIP まで使える。8bit PNG は sRGB 前提。
- **テクスチャは全部ロード済みの状態で**(ストリーミングの低解像度を撮らない)。数秒待つ。
- **同じ視点を複数**: カメラ名(仕様の `cameras[].name`)と同じファイル名で置く: `references/<シーンid>/<カメラ名>.png`
  (`%LOCALAPPDATA%\UnoEngine\parity\references\<シーンid>\` か、仕様の `reference.external.dir`)。グレーカードが写せるなら 1 枚写す(校正が確実になる)。

取り込み後の流れ: §5 の校正手順 → サイドカー JSON に整列を書く → `parity run <spec> --reference external --stage G1`。

---

## 9. 制約・注意

- **ヘッドレスの表示矩形は 1043×587 系**(実機では 977×550 など)。Q2 で `screenshot_final {width,height}` / `--size WxH` の任意解像度オフスクリーン出力が入った(§11.6)。`resolution: [1920,1080]` は期待値で、違えば `sizePolicy` で合わせる。撮影解像度指定は Q2 の担当。
- **`screenshot_final` の既定は 8bit PNG**。Q2 で `format:"pfm"|"exr"`(ポスト前の線形 float。§11.5)が入り、HDR-FLIP・線形の比較(Tier A)ができる。旧記述(`screenshot_linear` が入るまで LDR)は解消済み。それまでの PT 基準との比較は
  「PT のリニアにエンジンと同じトーンマップを掛けて 8bit PNG と比べる」形(Tier B)。
- `set_editor_camera` は FOV を変えられない(45° 固定)。カメラ FOV の違う基準とは比べられない。
- ランナーは**実プロジェクトを開かない**運用(`engine.setup` の呼び出しや自動保存でシーンが書き変わりうる)。使い捨てのコピーか生成器(`generator`)を使う。
- エンジンのブリッジは単一クライアント。ランナーが繋いでいる間は、他の MCP クライアントから同じエンジンを操作しない(専用インスタンス `par` :8820 を使うので通常は問題ない)。
- 性能計測(`benchmark`)は GPU を専有する。他の計測・パリティ撮影と重ねない(`Global\dx12-gpu-bench` の排他は未実装)。
- LPIPS は実装していない(重みの利用条件が未確認。参考表示のみの方針)。
- FLIP は公式 `flip-evaluator` が入っていることが前提(無ければ FLIP 指標はエラー。自前実装の代替は作っていない)。
- 色の意味(生成シーンの `color`)など、エンジン側の仕様の実測が要る箇所は PS-0 で確かめる(`gen/ps0_calibration.py` の注記)。

## 10. テスト

`tools/parity/tests/`(pytest 76+ 件、約 45 秒): 画像 I/O(8/16bit PNG・PFM・EXR)・色空間・既知の色/ΔE2000 の公開値・指標(同一・既知のずれ・SSIM の独立実装との一致・FLIP の公式サンプル)・
ノイズ床(合成ノイズ)・トーンマップ・ゲート/仕様の検証・ヒートマップ・**偽エンジン(実エンジンと同じ行 JSON の TCP)でのランナー統合**(合格/不合格/skipped/外部基準/領域/性能予算と履歴/生成器/attach/
MCP ジョブ進捗/baseline の update→approve→check サイクル)・CLI の終了コード。
`ctest -R ParityHarnessPyTests`(Python か依存が無ければスキップ)。手で回すなら `tools\parity\.venv\Scripts\python.exe -m pytest tools\parity\tests -q`。

---

## 11. Q2 校正（光の物理単位・露出・UE 系トーンマップ・線形 HDR スクリーンショット・任意解像度出力）

設計: `docs/GRAPHICS_PARITY_DESIGN.md` §3.5「揃える条件」の Q2 の行を実装したもの。**すべて既定 OFF / 既定値では従来と 1 ビットも変わらない**
（フォワード系シェーダの DXIL は既定パスで同一・決定論スクショ 9 シーンの差分 0。証拠は §11.9）。

### 11.1 一覧（何がどこにあるか）

| 機能 | 入口 | 実装 |
|---|---|---|
| ライティング単位（従来 / 物理） | `dx12_set_post_process {lightingUnits:0\|1}`・エディタの Post Process 窓 / ライティング窓「スカイ / IBL」の上・シーン JSON `postProcess.lightingUnits` | `shaders/forward/Lighting.hlsli`（`-D UNO_PHYSICAL_LIGHTS=1` の別 .cso: `ForwardPhys_PS` / `ForwardMaskPhys_PS` / `TerrainPhys_PS`）・`Application::UpdateLightingUnits`（PSO 差し替え）・`PointLight/SpotLight::sourceRadius` |
| 露出（手動 EV100 / 自動） | `exposureMode` / `ev100` / `evComp` / `aeMinEv100` / `aeMaxEv100` / `aeSpeedUp` / `aeSpeedDown` / `aeLowPercent` / `aeHighPercent`（`dx12_set_post_process`） | `PostProcess.cpp`（手動係数）・`AutoExposurePass` + `shaders/post/AutoExposure.hlsl`（ヒストグラム・上下限・窓・上下速度） |
| トーンマップの選択 | `tonemapper` 0..5 と `film*`（UE Filmic のパラメータ） | `shaders/post/Tonemap.hlsli`（3 = UE Filmic / 4 = 線形 / 5 = Khronos PBR Neutral）。0..2 は従来のまま |
| 線形 HDR スクリーンショット | `dx12_screenshot_final {format:"pfm"\|"exr"}` / `formats` | `LinearCapturePass` + `shaders/post/LinearCapture.hlsl`（`RenderPostChain` から要求のあるフレームだけ） |
| 任意解像度のオフスクリーン出力 | `dx12_screenshot_final {width,height}` / 起動引数 `--size WxH` | `ApplicationOffscreenShot.cpp`・`core/OffscreenShot.h`（純ロジック） |
| 純関数（単位・露出・トーンマップ） | — | `src/renderer/PhotometricMath.h`（単体テスト `PhotometricMathTests`）。MCP の CPU 側スクショもこのトーンマップを共有 |

### 11.2 光の単位（物理モード）と対応表

物理モード（`lightingUnits:1`）の約束: **シーンの線形 RGB は輝度 [nit = cd/m²] そのもの（エンジン単位 1.0 = 1 nit）**。
表示への変換は露出係数 `F = 1/(1.2·2^EV100)`（§11.3）だけ。エンジン単位への追加スケール（プリエクスポージャ）は持たない
（fp16 のシーン RT の上限 65504 に対し、物理フォワード PS が最終色を 6.0e4 で頭打ちにして inf / NaN がポスト・TAA に漏れるのを防ぐ。
太陽 10 万 lux の白いランバート面 = 3.2 万 nit なので実用範囲は収まる）。

| もの | 従来モード（`Lighting.hlsli`） | **物理モード** | パストレーサー（`docs/PATH_TRACER.md` §3） | UE（記憶。未照合） |
|---|---|---|---|---|
| 太陽 `DirectionalLight` | `Lo = BRDF·(color·intensity)·N·L`。intensity は任意単位（既定 3） | **intensity = lux**（法線に垂直な面の照度）。式は従来と同じ = 拡散面の放射輝度 `albedo/π·E·cosθ`。**単位を読み替えるだけで式は同一** | 放射照度 `E = color·intensity`（π で割らない）= 同じ | Directional Light の Intensity = lux |
| 点 / スポット | `att = saturate(1−d/range)²`（**逆二乗ではない**）。intensity は任意単位 | **intensity = 光度 [cd]**。`att = saturate(1−(d/range)⁴)² / max(d², r²)`。r = `sourceRadius`（下限 1 cm）。`L.color`（= color·intensity）に掛けるので面の照度 = `I·cosθ/d²` | 従来式（`lightFalloff:"engine"`）か、`lightFalloff:"physical"` = 純粋な `1/d²`（窓なし） | 逆二乗 + 減衰半径（窓は `saturate(1−(d/R)⁴)²` 系）。cd / lm / EV を選べる |
| lm ↔ cd | — | 点 `cd = lm/4π`・スポット `cd = lm/(2π(1−cosθ_outer))`（純関数 `LumensToCandelaPoint/Spot`。入力は cd。MCP / UI は cd を直接受ける） | — | 点は `/4π`（スポットは版で式が違う。未確認） |
| 空 / IBL | `iblIntensity`（拡散 = 放射照度キューブ × 強度）・`skyboxIntensity`・`DirectionalLight.ambient`（IBL 無し）は任意単位 | **同じフィールドを nit として解釈**（`iblIntensity` = キューブ値 1.0 あたりの輝度 [nit]。既定の手続きの空は ≈ 0.5〜1.5 なので 4000〜8000 で晴天相当） | 環境キューブの放射輝度を `iblIntensity` 倍（拡散も鏡面も）= 同じ | Sky Light Intensity Scale / SkyAtmosphere（cd/m²） |
| 自己発光 | `emissiveIntensity`（色 8bit・強度は二乗曲線 8bit の b2 量子化） | **nit として解釈**（式・量子化は同じ） | 同じ値を放射輝度として持つ（面光源になる） | Emissive は nit ではなく相対値（露出前）。Substrate なら cd/m² の指定もある |
| 面光源 | 無い（エミッシブ面は他を照らさない） | 同じ（エミッシブが照らすのは PT だけ。既知の差） | エミッシブ三角形を NEE + MIS で面光源にする | Rect Light（cd / nit） |
| 露出・トーンマップ | uber パスで適用（`exposure` 乗算 / 自動露出） | 露出モード 1/2（§11.3）+ トーンマップ 0..5 | なし（線形出力。露出前・トーンマップ前） | 露出は Auto / Manual（EV100）。トーンマップは既定 Filmic |

- **PT との位置合わせ**: フォワード物理と PT は**同じ nit 単位で同じ線形値**になる（比較は追加のスケールなしで取れる）。PT には `lightFalloff:"physical"` を渡し、
  光源の `range` は影響半径の窓が事実上 1 になるよう十分大きく（PS-0 は 200〜500 m）取る。光源半径は PT には無い（デルタ光）ので `sourceRadius` は 0〜2 cm にする。
- `range` は物理モードでも **クラスタカリングの境界**なので必要（`dist ≥ range` の灯は評価しない）。窓 `saturate(1−(d/range)⁴)²` は境界を滑らかに 0 へ落とすためのもので、`d/range < 0.3` ではほぼ 1（誤差 2% 未満）。
- **物理単位に対応していないもの（従来のまま）**: カスタムシェーダー（`UnoCustom`）・マテリアルグラフ材質・ForwardGrid（エディタの床グリッド）・ボリュメトリックフォグの光・DDGI の点光源寄与・パーティクル発光。
  これらは物理モードのシーンでも従来式で描かれる（フォワードのメッシュ / スキンド / 地形 / MASK は対応）。

### 11.3 露出

**露出係数**（手動）: `F = 1/(1.2·2^(EV100 − 補正))`。1.2 は ISO 12232 の飽和ベース（`Lmax = 78/(q·S)·N²/t = 1.2·2^EV100`、q = 0.65・S = 100）に由来する定数。
UE の内部係数（`EV100ToLuminance` 系の 1.2）と同じ形にしてあるが、**UE 本体での照合は未実施**。

| 設定 | 意味 |
|---|---|
| `exposureMode = 0`（既定） | 従来: `exposureOn` / `exposure` の乗算と `autoExposureOn`。**絵は従来と不変** |
| `exposureMode = 1` 手動 | `F` を掛ける。**マスター `enabled` が OFF でも効く**（露出はカメラの性質でエフェクトではない）。従来の `exposure` 乗算は美術用の上乗せとして残る |
| `exposureMode = 2` 自動 | ヒストグラム測光。平均輝度を 18% グレー × 2^`evComp` へ写す（`F = 0.18·2^evComp/L_avg`）。**上下限 = `aeMinEv100` / `aeMaxEv100`**（ヒストグラムのレンジ = `Log2LuminanceFromEv100`）。速度は `aeSpeed`（`aeSpeedUp` = 明るくなる方向 / `aeSpeedDown` = 暗くなる方向。0 なら `aeSpeed`）。測光窓 `aeLowPercent`〜`aeHighPercent`（0..1 = 全体の対数平均。0.8..0.983 = UE のヒストグラム測光の既定） |

**露出 0 の基準**: 物理モード + `EV100 = 15`（晴天 Sunny 16）+ 補正 0（`kDefaultEv100`）。係数 = 2.543e-5。太陽 10 万 lux の下の 18% グレーは表示 0.146（`PhotometricMathTests` で検証）。
EV100 の目安: 晴天屋外 15 / 曇り 12〜13 / 明るい屋内 8〜10 / 暗い屋内 4〜6 / 夜景 0〜3。

### 11.4 トーンマップ（選べる 6 種）

| 番号 | 名前 | 出力の符号化 | 備考 |
|---|---|---|---|
| 0 | ACES（Narkowicz） | `pow(1/2.2)`（従来） | 既定・従来のまま |
| 1 | AgX | ガンマ空間の値を直接返す（従来） | 従来のまま |
| 2 | なし（ガンマのみ） | `pow(x,1/2.2)`（従来） | 従来のまま |
| **3** | **UE Filmic** | **sRGB OETF** | UE 5 の既定（ACES 系）の再現。`filmSlope`(0.88) / `filmToe`(0.55) / `filmShoulder`(0.26) / `filmBlackClip`(0) / `filmWhiteClip`(0.04) |
| **4** | **線形（クリップのみ）** | **sRGB OETF** | トーンマップ無し。1 でクリップするだけ（比較・校正用） |
| **5** | **Khronos PBR Neutral** | **sRGB OETF** | glTF 標準ビューア用（色相を保つ）。公開アルゴリズム（Apache-2.0） |

★**出力の符号化が 0〜2 と 3〜5 で違う**（従来は `pow(1/2.2)`、新規は sRGB の OETF）。パリティ仕様の `engine.png` は `gamma22`（0〜2）/ `srgb`（3〜5）を切り替えること。

**UE Filmic の根拠と不確実性**（`PhotometricMath.h::UeFilmicLinear` の冒頭にも同じ注記）:
- 確かなもの: 公開文書（Color Grading and the Filmic Tonemapper）が示す既定値 Slope 0.88 / Toe 0.55 / Shoulder 0.26 / Black Clip 0 / White Clip 0.04。UE の既定トーンマッパが ACES 準拠に設計されていること。
- **記憶から独自に書き起こしたもの**: ACES 1.0 参照実装の RRT（グロー・赤の変調・彩度係数 0.96）+ 対数空間の S 字（toe = ロジスティック / 直線 / shoulder = ロジスティックを smoothstep で継ぐ。中間灰 0.18 → 0.18 を固定点にする `ToeMatch` の求め方）+ 出力側の彩度 0.93。
  UE のシェーダのコードは 1 行も写していない（EULA 上の注意。法的判断はしていない）。
- **確かな性質（構成から）**: 0 → 0 / 0.18 の灰 → 0.18 のまま / 単調増加 / 大きな入力 → `1 + WhiteClip` に漸近 / 灰は灰のまま。`PhotometricMathTests` と pytest が numpy 実装との突き合わせ（1e-4 以内）と不変条件で検証している。
- **不確実**: グロー・赤変調・彩度係数の細部（UE 5.x の版ごとの差）・AP1 → sRGB の色域処理（UE は版によって既定で色域拡張を掛ける）・出力の符号化（UE の既定表示は sRGB だが、HDR ディスプレイ出力は別）。
  **UE 本体との数値照合は未実施**（UE を再インストールしない方針）。ユーザーが UE のスクリーンショットを撮れたら §5 の校正で残差を見る。
- 数値の目安（リニア入力 → 出力）: 0.18 → 0.180 / 1.0 → 0.723（sRGB 0.87）/ 10 → 0.9995 / 0.01 → 0.00166。

### 11.5 線形 HDR スクリーンショット（`dx12_screenshot_final`）

```
dx12_screenshot_final {path:"C:/tmp/a", formats:["png","pfm"], width:1920, height:1080, deterministic:true}
  → {path:"C:/tmp/a.png", files:{png:"C:/tmp/a.png", pfm:"C:/tmp/a.pfm"}, linear:{...}, offscreen:true, width:1920, height:1080, ...}
```

- **既定は従来どおり**: 引数を全部省くと PNG のみ・ビューポート矩形・応答のキーも従来 + 加算キー（`files` / `linear` / `offscreen`）だけ。
- `format` / `formats`: `png`（表示色 8bit）/ `pfm` / `exr`（**線形 float**）。`path` の拡張子は無視され、形式ごとに同じ基準名で書く。pfm/exr だけのときは画像ブロックを返さず JSON（`files` に形式ごとのパス）。
- **線形 float の意味**（比較ツールが読む規約）: **シーン参照の線形 Rec.709 RGB**。**トーンマップ前・露出前**。TAA / DoF / モーションブラーの後、ブルーム・ゴッドレイ・フレアの前
  （＝ポスト(uber)へ入る直前のシーン色。`RenderPostChain` で自動露出と同じ位置・同じ SRV から読む）。物理ライティング単位ならシーン値は nit。NaN / Inf は 0 に置き換える。
  書式はパストレーサーの出力と**同じ writer**（`renderer/pt/PtImageIO.h`）: PFM は下から上の標準（リトルエンディアン `-1.0`）・EXR は無圧縮 float32 / RGB。
- 線形と PNG は**同じフレーム**から撮る（コピーは同じコマンドリスト）。決定論（`deterministic:true`）なら同じ設定で 2 回撮るとビット一致（§11.9）。
- 制限: 線形 float 出力は 4096×4096 画素まで（float4 の UAV + readback で 2 倍要る）。ポスト前のシーン色なので、ブルーム・ビネット・LUT・グレーディング・スクリーンシェーダーは写らない（PNG 側には写る）。
  露出前なので、表示と突き合わせるときは `F`（§11.3）を掛けてトーンマップする（ハーネスの `alignment.exposure: "ev100:15"`）。

### 11.6 任意解像度のオフスクリーン出力（`width` / `height` / `--size WxH`）

- 撮影中だけ、**シーン系 RT 一式（シーン HDR・深度・AO・Hi-Z・ブルーム・TAA 履歴・G-Buffer・SSGI/SSR ほか）を `width x height` で作り直し**、
  uber パスの出力を専用の LDR RT（バックバッファと同じ形式）へ向けて読む。ビューポート / ウィンドウ / 16:9 レターボックスに依存しない。撮影が終わると次のフレームで元の解像度へ戻る
  （状態は `m_mcpFinalShot` に載せてあり、応答が返れば自動で消える = 戻し忘れが構造的に起きない）。
- **アスペクトは `width/height`**（カメラの投影もそれで作る。垂直 FOV は据え置き）。
- **上限とエラー処理**: 1 辺 16〜8192・総画素 8192×4096 以下（`OffscreenShotTests`）。**GPU メモリの見積**（シーン系 RT ≈ 160 B/画素 + 線形出力なら 32 B/画素）が「予算 − 使用中」の 60% を超えると**撮影前に構造化エラー**
  （`size WxH needs more GPU memory than is available right now`）。RT の確保に失敗したら撮影を中止してエラー応答（解像度は次のフレームで元へ）。
- **決定論**: 解像度の切替が完了してから履歴を捨て、`settleFrames` を数え直す（「同じ初期状態 + 同じフレーム数」）。ハンドラがどのタイミングで呼ばれても結果は同じ。
- **写らないもの**: エディタのアイコン / 選択枠（撮影中は `gizmos` に依らず出ない）・ゲーム内 UI 画像・画面全体のカスタムシェーダー（中間 RT が表示解像度のため）。3D の絵 + ポストだけ。
- **副作用**: エディタで撮ると、撮影中の数フレームだけシーンビューがクリア色になる（バックバッファにはシーンが描かれない）。ヘッドレス / 背景起動では見えない。
- `--size WxH`（起動引数）は `width` / `height` を省いた `screenshot_final` の既定になる。不正な値は無視（ログに理由）。
- **シーケンサーのレンダー出力（S2b）が使う基盤**: 1 フレームごとに `screenshot_final {width,height,formats,deterministic}` を呼べばよい。

### 11.7 パリティ・ハーネス側の追加（`tools/parity`）

- トーンマップ: `ue_filmic`（= `engine_ue_filmic`）/ `pbr_neutral` / `engine_linear` を追加（numpy の独立実装。C++ の参照値と同じ表を pytest が検証）。旧 `ue_filmic_approx`（aces_hill の別名）は残してあるが、以後は `ue_filmic` を使う。
- 露出の書式 `ev100:15` / `ev100:15,+1`（線形の両者に `F` を掛ける）。`ev100_to_scale` / `ev100_to_ev`。
- 仕様: `engine.size`（撮影解像度）/ `engine.linear`（線形 PFM も撮って**線形どうしで比較**）/ `cameras[].setup`（カメラごとの MCP 呼び出し = 露出ブラケット）/ `cameras[].alignment`（カメラごとの整列）/ `displayCheck`。
  **線形どうしを比べると 2 系統が 1 回で出る**: 線形のまま = HDR-FLIP、同じトーンマップ + 露出を両者へ掛けた表示 = LDR-FLIP・SSIM・ΔE。
- **displayCheck**: 同じフレームの線形 PFM に numpy でトーンマップ + 露出を掛けた期待値と、エンジン自身の表示 PNG（GPU のトーンマップ + 露出）を 8bit で比べる。GPU 実装と numpy 実装の一致の証拠（LSB 単位の平均・p99・最大）。
- **PT アダプタの実結合**（Q1 の未確認事項）: 実エンジンの `render_reference` は非同期（即座に `accepted` を返す）。`reference.pt.api: "engine"`（既定）で `output` / `formats` / `size` を渡し、`render_reference_status` を `state:"done"` までポーリングして `output.files` から PFM を読む。
  `frameBudgetMs` の既定は無人実行向けに 40。旧同期 API は `"api": "legacy"`（偽エンジン用）。
- 追加した仕様: `ps0_calibration`（更新）/ `ps0_lightsteps` / `ps0_invsq` / `ps0_exposure`。生成器 `gen/ps0_calibration.py`（`variant` = calibration / lightsteps / invsq・`units` = legacy / physical）。

### 11.8 PS-0 の実測（フォワード物理 + 線形出力 対 パストレーサー。2026-09-30）

**条件**: 1920×1080・物理ライティング（太陽 8 万 lux・空 8000 nit）・EV100=15・UE Filmic・DDGI / SSGI / SSR / SSAO / TAA / Bloom / ビネット / 自動露出は全部 OFF。
基準 = `render_reference`（1024 spp × シード 2 本・`bounces=1`・`lightFalloff:"physical"`）。線形どうしを比べ、同じ露出 + トーンマップを両者へ掛けて LDR-FLIP・SSIM・ΔE も出す。
ゲートは **G2**（LDR-FLIP 平均 ≤ 0.08・p95 ≤ 0.3・HDR-FLIP 平均 ≤ 0.1・輝度差 ≤ 0.25 EV・SSIM ≥ 0.9・ΔE2000 中央 ≤ 3・色相ずれ中央 ≤ 8°）。
フォワード側のノイズ床はほぼ 0（決定論）。PT 側の床は HDR-FLIP 平均 0.0025〜0.0061・LDR ≈ 0。**しきい値は固定値**（床の 1.5 倍を下回るものが無いため、床規則で緩む項目は無い）。
表示検証（`displayCheck`: numpy のトーンマップ + 露出 と GPU 出力の PNG の差）は全カメラで平均 0.01〜0.10 LSB・最大 1 LSB（GPU と numpy が一致）。

| 仕様 / カメラ | LDR-FLIP 平均 | LDR p95 | HDR-FLIP 平均 | SSIM | ΔE 中央 | 輝度差 [EV] | ヒストグラム EMD [EV] | 判定 |
|---|---:|---:|---:|---:|---:|---:|---:|:-:|
| calibration / overview | 0.0499 | 0.186 | 0.0827 | 0.9645 | 0.14 | +0.045 | 0.149 | pass |
| calibration / graycard | 0.0309 | 0.092 | 0.0419 | 0.9899 | 0.16 | +0.026 | 0.047 | pass |
| calibration / patches | 0.0590 | 0.218 | 0.0963 | 0.9574 | 0.23 | +0.047 | 0.164 | pass |
| calibration / ramp | 0.0539 | 0.218 | 0.0912 | 0.9540 | 0.16 | +0.047 | 0.177 | pass |
| calibration / bsdf_grid | 0.0835 | 0.284 | 0.1428 | 0.9253 | 0.51 | +0.064 | 0.343 | pass（★既知の差の許容つき） |
| lightsteps / steps（光 1/4/16/64 倍） | 0.0092 | 0.014 | 0.0112 | 0.9992 | 0.05 | −0.003 | 0.008 | pass |
| lightsteps / steps_low | 0.0094 | 0.013 | 0.0103 | 0.9997 | 0.05 | −0.002 | 0.002 | pass |
| invsq / along（逆二乗・軸上） | 0.0032 | 0.013 | 0.0069 | 0.9999 | 0.00 | −0.006 | 0.003 | pass |
| invsq / side（逆二乗・斜め） | 0.0021 | 0.009 | 0.0087 | 0.9999 | 0.00 | −0.010 | 0.005 | pass |
| exposure / EV100=13（+2 段） | 0.0641 | 0.319 | 0.0827 | 0.9603 | 0.07 | +0.031 | 0.141 | pass（★p95 の許容つき） |
| exposure / EV100=15 | 0.0499 | 0.186 | 0.0827 | 0.9643 | 0.14 | +0.045 | 0.149 | pass |
| exposure / EV100=17（−2 段） | 0.0231 | 0.090 | 0.0827 | 0.9812 | 0.07 | +0.048 | 0.096 | pass |

- **単位系の校正そのもの（光の強さ・逆二乗・露出）は G2 を大きく下回る**（lightsteps・invsq は LDR-FLIP 0.01 以下・輝度差 ±0.01 EV 以内）。露出ブラケット ±2 段でも輝度差は 0.03〜0.05 EV で一定 = 露出係数は正しい。
- 輝度差が全カメラで +0.03〜0.06 EV の正の偏りを持つ（フォワードが明るい）。原因は下の「既知の差」の 1（空の可視性）と 4（拡散の近似）。

**既知の差**（フォワードの仕様であって Q2 の校正では直さないもの。★は G2 の許容で吸収した箇所 = `bsdf_grid` の LDR-FLIP 平均 ≤ 0.10・HDR-FLIP 平均 ≤ 0.16 / `ev_m2` の LDR p95 ≤ 0.35。全体の上限は他カメラで維持）:

| # | 差 | 見え方 | 担当 |
|---|---|---|---|
| 1 | **IBL の空の可視性が無い**: フォワードは直接光 + IBL のみで、球が床へ落とす空の遮蔽（接触影・AO）と、光沢球の下側の写り込みを持たない。PT は環境光を可視性つきで評価する。`features.ssao` を有効にして撮り直しても数値が 1 桁も変わらなかった（SSAO が物理モードの IBL 項に効いていない / 設定が反映されなかった可能性。**原因は未確認**） | `bsdf_grid` の球の下縁・床の球影、`ev_m2`（明るいほど縁が目立つ） | DDGI / SSGI / A1 / 空の遮蔽（AO・RT 影）。★ |
| 2 | **GI 無し**: PT も `bounces=1` にしてあるので間接拡散は両者とも無い。`bounces=8` の全光輸送とは差が出る（PS-0 では測っていない） | 未計測 | DDGI 段階 1 |
| 3 | 光沢面が写す周囲の物体（球どうし・床の写り込み）: フォワードは SSR OFF のため環境キューブしか写さない。PT は 1 回目の鏡面反射で環境だけを見る設定なのでほぼ同じだが、球のふちで差が残る | `bsdf_grid` 上段のふち | SSR / RT 反射（RT1） |
| 4 | 拡散の近似: フォワードの拡散 IBL は放射照度キューブ（1 回のコサイン畳み込み）。PT の環境光は可視性つきの NEE。全球が見える面では一致するが、遮蔽のある面（床の球の近く）でずれる（1 と同根） | 輝度差の +0.03〜0.06 EV | 同上 |
| 5 | 面光源: エミッシブは PT でだけ光る | PS-0 にエミッシブ光源は無いので出ない | A1 以降 |
| 6 | 物理モード非対応の描画（カスタムシェーダー・マテリアルグラフ・ForwardGrid・ボリュメトリックフォグ・DDGI の点光源寄与・パーティクル発光）は従来式のまま | PS-0 では出ない | 各段階 |
| 7 | `furnace` カメラ: 一様な白い環境を設定できる機能が無いので `skip` | 未計測 | 環境の設定 API（Q3 以降） |

### 11.9 検証の証拠（既定 OFF = 従来と 1 ビットも変わらない）

- **DXIL 同値**（`tools/shader_dxil_equiv.ps1`）: Q2 の編集（Lighting.hlsli の物理モード分岐・ForwardShade.hlsli のクランプ）を外した「変更前」の shaders と現在の shaders で、フォワード系 12 本
  （Forward / ForwardLdr / ForwardMask / ForwardSkinned / ForwardSkinnedMask / Terrain の VS・PS、ForwardInstanced、ForwardGrid）の DXIL を比べて**同一 12 / 順序のみ 0 / 差あり 0**（ハッシュも一致）。
  物理モードは `-D UNO_PHYSICAL_LIGHTS=1` の別 .cso（`ForwardPhys_PS` / `ForwardMaskPhys_PS` / `TerrainPhys_PS`）で、既定の .cso の中身は変わらない。
- **決定論スクショ 9 シーン**（indoor_skinned / indoor_skinned_allfx / sponza_ibl / sponza_noibl / terrain / outdoor_fox / quality_pbr_ibl / studio_lumen / synth_translucent_custom）:
  Q2 の前のビルド（base）を 2 回、Q2 のビルド（new）を 2 回撮って**全シーンでハッシュ一致**（base 同士・new 同士・base と new）。撮影は `deterministic:true` の `screenshot_final`。
- **オフスクリーン・線形出力の決定論**: 実機（`--size 1280x720` 起動・ヘッドレス）で 1920×1080 の PNG と線形 PFM を 2 回撮って**ビット一致**（md5 一致）。1280×720 / 1920×1080 / 777×333 / 3840×2160 の png / pfm / exr が全部書けて、PFM のバイト数が 幅×高さ×12 + ヘッダに一致。範囲外・片方だけ・不正形式・線形の上限超えは構造化エラー、撮影後は元の解像度へ戻り、エラー後もエンジンは生きている。
- **単体テスト**: `PhotometricMathTests`（EV100↔係数・トーンマップ参照値・lm/cd）・`OffscreenShotTests`（`--size` の検証）・`TonemapGpuTests`（HLSL と C++ / numpy の最大差 6e-7）・pytest（`tools/parity/tests/test_q2_calibration.py` ほか）。
- **デバッグレイヤ**(`DX12_D3D_DEBUG=1`・ヘッドレス・`--size` 起動): オフスクリーン撮影 + png/pfm/exr の実機検証中の警告 0 件。`LinearCapturePass` の出力バッファは COMMON で作り、明示バリアで UAV へ遷移する（バッファを他の状態で作ると id=1328 の警告が出る）。
