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

- **ヘッドレスの表示矩形は今 1043×587 系**(実機では 977×550 など。`--size` 未実装)。`resolution: [1920,1080]` は期待値で、違えば `sizePolicy` で合わせる。撮影解像度指定は Q2 の担当。
- **`screenshot_final` は 8bit PNG**。HDR-FLIP・線形の比較(Tier A)は、リニア float スクショ(Q2 の `screenshot_linear`)が入るまで**エンジン側が LDR**。それまでの PT 基準との比較は
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
