# 描画の回帰テスト（ゴールデン画像）

固定シーン × 固定カメラを**決定論スクショ**で撮り、`tests/golden/*.png` と比べる。
実体は `tools/bench/golden.mjs`、設定は `tests/golden/golden.json`、ネイティブ解像度の画素ハッシュは `tests/golden/baseline.json`。

## 回し方（ローカル）

```
run_golden.bat                 # 純関数のテスト → 4 シーンを撮って比較
run_golden.bat --update        # 意図した絵の変更のときだけ基準を撮り直す
run_golden.bat --only nocturne_hall --repeat 2   # 決定論も確かめる
```

- exe: `DX12_ENGINE_EXE` → `build/dev/DX12Engine.exe` → `build/release/...` の順で最初にあるもの（`--exe` で明示も可）。
- シーンの `project` は手元のゲーム（PerfTest / Nocturne / Junction）を指す。見つからないシーンは **SKIP**（`--strict` で不合格）。
  別の PC では `DX12_GOLDEN_PROJECTS_ROOT=<3 つの親フォルダ>` を渡す。リポジトリ内の相対パスも指定できる。
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
- 今ある組み合わせ: DPI 100% × テーマ default / b × 既定レイアウト。DPI 150%、マテリアル / ライティング WS のレイアウトは必要になったら `golden.json` へ足す（`dpi`・`theme` で指定。レイアウトは未実装）。
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
