# 描画の回帰テスト（ゴールデン画像）

固定シーン × 固定カメラを**決定論スクショ**で撮り、`tests/golden/*.png` と比べる。
実体は `tools/bench/golden.mjs`、設定は `tests/golden/golden.json`、ネイティブ解像度の画素ハッシュは `tests/golden/baseline.json`。

## 回し方（ローカル）

```
run_golden.bat                 # 純関数のテスト → 3D（core_*）+ UI を撮って比較
run_golden.bat --update        # 意図した絵の変更のときだけ基準を撮り直す
run_golden.bat --only core_pbr_spheres --repeat 2   # 決定論も確かめる
run_golden.bat --group repeat --repeat 2             # DDGI/DXR の別枠（core_new_scene_gi）
```

- exe: `DX12_ENGINE_EXE` → `build/dev/DX12Engine.exe` → `build/release/...` の順で最初にあるもの（`--exe` で明示も可）。
  起動は `tools/engine_instance.ps1` 経由（`--headless`）。exe / DLL / shaders / assets をインスタンス専用フォルダ
  （`%LOCALAPPDATA%/UnoEngine/agent-instances/golden-3d-<リポジトリパスのハッシュ>`）へコピーしてから起動するので、ビルド出力を握らず次のビルドが LNK1104 にならない。
- **`core_*`（リポジトリ内で完結）**: `tests/golden/projects/golden_core`（プリミティブと手続きの空だけ）の 5 シーン
  （PBR 球の列 / CSM / 半透明 / ポスト・発光 / ロード時の既定値 = 設定ブロックの無い手書きシーン）。別枠 `core_new_scene_gi` は実際の new_scene の結果（下記 group:"repeat"）。どの PC でも必ず回る。シーンは `gen_scenes.mjs` で再生成する。
- **手元のゲーム由来の 4 件**（perftest_ddgi_probe / perftest_stress_5000 / nocturne_hall / junction_f6）は `requiresProjectsRoot:true`。
  環境変数 `DX12_GOLDEN_PROJECTS_ROOT=<PerfTest/Nocturne/Junction の親フォルダ>` があるときだけ回し、無ければ**対象から外す**（SKIP の見かけ合格にしない）。
  基準画像は撮った PC のもの（旧 launchEngine で 1043x587）。★今の launchEngine は engine_instance.ps1 経由で 977x550 なので、そのプロジェクトがある PC で一度 `--update` して撮り直すまでは環境違い（strict では不合格）になる。
  `--only` はこの絞り込みの後に効く（環境変数なしで `--only nocturne_hall` と名指ししても「対象のシーンが無い」）。
- 既定は `--strict`（プロジェクトが見つからないシーンは不合格）。緩めるのは `--no-strict`。
- `group:"repeat"` のシーン（DDGI / DXR など決定論が崩れるもの）は通常実行に含めず、`--group repeat` で別枠に回す。今は `core_new_scene_gi` が該当: 実際の `new_scene` コマンド（Grid + Ground + Cube + DirectionalLight + MainCamera + `gi::ApplyNew` = DDGI 自動配置 / 環境光 0 / SSGI / RT 影）の結果。新規シーンの既定プリセットを変える作業はこれで検出する（`scene` ではなく `newScene:true`）。`requiresDxr:true` が付いており、インライン RT 非対応のハード（`get_dxr` の `raytracingTier` が 1.1 未満、または `highestShaderModel` が 6.5 未満）では撮らずに SKIP になる。対応ハードなのに `supported:false` のとき、または `get_dxr` が例外のときは FAIL（回帰扱い。新 GI が使えない環境の絵は基準と別物になるため）。
- 新しい `core_*` の基準は RTX 5060 で `--update --repeat 2` して撮った（同一セッションで 2 枚が 1 ビットも違わないことを確認）。ネイティブ解像度は 977x550。
- 出力は `build/golden-out/`（撮った絵・`*.diff.png`。違う画素は赤）。
- 純関数（縮小・差分・判定・プロジェクト解決）だけなら GPU 不要: `node --test tools/bench/golden.test.mjs`。

## 判定

1. `bitExact` … ネイティブ解像度の SHA-256 が基準と同じ（参考情報）。
2. 合否 … 640x360 へ面積平均で縮めて画素ごとに比較。違う画素 0.1% 以下かつ平均差 0.25 以下で合格。

★基準は撮った PC の GPU / ドライバに依存する。別の GPU では 1 LSB 以上ずれ得るので、
CI で回す場合は基準を撮った GPU と同じランナーで回すこと（GitHub-hosted の Windows には GPU が無く、エンジンもビルドできない）。

## UI ゴールデン（kind:"ui"。大改修の安全網）

エディタ UI（ImGui 込みの最終画面）の見た目が意図せず変わっていないかを見る。3D のゴールデンとは別枠で、`golden.json` の `kind:"ui"` のシーンが対象。

```
node tools/bench/golden.mjs --ui-only                  UI だけ基準と比べる
node tools/bench/golden.mjs --ui-only --update         UI の基準（tests/golden/ui/*.png）を撮り直す
node tools/bench/golden.mjs --ui-only --force-theme b  default のシーンを --theme-variant b で撮って比べる（差分を検出できる確認用。FAIL が正しい）
node tools/bench/golden.mjs --no-ui                    3D のゴールデンだけ回す
```

- 起動は `tools/engine_instance.ps1` 経由の **`--background`**（exe のコピー・窓は人の画面に出ない・実マウスに触れない）。データ領域と `imgui.ini` はインスタンス専用なので、人の設定に左右されず**既定レイアウト**で始まる。
- 決定論は 3 つで作る:
  1. `--ui-deterministic`: UI の時計 `ui::clock::Now()`（`src/gui/UiClock.h`）が「フレーム番号 × 1/60」になり、`io.DeltaTime` も 1/60 固定、キャレットは点滅せず、仮想カーソルは描かれない。W1-A2 の `ui::anim` もこの時計を使う。
  2. `imgui_screenshot {hideCursor:true}`: 仮想カーソル（矢印・波紋）を絵に写さない。
  3. `step_frames` で warmup（既定 120 フレーム）してから撮る。
- 同じセッションで毎回 2 枚撮り、ネイティブ解像度で **1 ビットも違わない**ことを確かめる（違えば FAIL。`build/golden-out/<name>.repeat-diff.png`）。
- 比較は 3D と同じ（640x360 へ面積平均で縮め、同じ許容差）。揺れる領域（トースト・時刻）は `masks: [[x0,y0,x1,y1]]`（0..1 の割合）で塗りつぶしてから縮める。
- 撮影用プロジェクトは `tests/golden/ui_project`（`--new-project --template empty` で作った最小構成）。実行のたびに `build/golden-out/ui_project` へコピーして使う。
- 組み合わせ: DPI 100% / 150% × テーマ default / b × レイアウト default / material / lighting の 12 通り（`golden.json` の `dpi`・`theme`・`layout`）。material / lighting は撮影前に `editor_command_run {id:"workspace.material"}` などで切り替える。material は、マテリアルグラフ窓の GPU パス（RenderGpu）が RT を戻さず ImGui が何も描かれなかった不具合の回帰検出を兼ねる（C3）。
- ★**カバーしていない範囲**: 各シーンは 3D ビューポート全面（約 31%）を `masks` で塗りつぶしている（DDGI / SSGI / RT 影のノイズが ±1 LSB 揺れるため）。ビューポート内のオーバーレイ（ライト・カメラ・音源アイコン、選択の描画）は**このゴールデンでは検出できない**。別途、決定論なシーン（GI / RT 影を切った撮影専用シーン）か、3D 画像だけを覆うマスクの検討が要る。
- 複数 worktree で同時に回しても衝突しないよう、インスタンス名は `golden-ui-<リポジトリパスのハッシュ>`。同じ worktree で 2 本同時に回すのは不可。
- 基準 PNG は 2880x1620（DPI 150% の native）などで撮っている。baseline.json の `commit` は撮影時の HEAD（dpi100 の 2 件はキャプチャ処理の追加前の親コミットを指しているが、基準画像の内容とは無関係）。
- 環境（GPU・ドライバ・フォント）が変わると基準とずれうる。ネイティブ解像度が違うときは SKIP（`--strict` で不合格）。

## CI への組み込み案（自前 GPU ランナー）

`.github/workflows/` はクラウドのセッションから push できないため、ここに案だけ置く。

```yaml
  golden-render:
    name: Golden render (self-hosted GPU)
    runs-on: [self-hosted, windows, gpu]
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-node@v4
        with: { node-version: '24' }
      - name: Build engine
        run: |
          cmake --preset windows-release
          cmake --build --preset windows-release
      - name: Golden
        env:
          DX12_GOLDEN_PROJECTS_ROOT: D:\golden-projects
        run: run_golden.bat --strict
      - uses: actions/upload-artifact@v4
        if: always()
        with: { name: golden-out, path: build/golden-out }
```
