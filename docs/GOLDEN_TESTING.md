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
