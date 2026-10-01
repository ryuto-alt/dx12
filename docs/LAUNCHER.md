# プロジェクトランチャー（起動後のプロジェクト選択・作成画面）

UE5 のプロジェクトブラウザ風の画面。左に縦ナビ、中央にカードのギャラリー、右に選択中の詳細パネル。
世界観はダーク + ネオングロー（起動画面と同じ配色・アクセント #2F8CFF）。

| ファイル | 役割 |
|---|---|
| `src/editor/LauncherScreen.{h,cpp}` | 画面（ImGui の全画面ウィンドウ 1 枚を ImDrawList で自前描画）。Editor ライブラリ |
| `src/project/LauncherLogic.h` | 純ロジック（テンプレ登録表 / 名前・保存場所・Git URL の検証 / 最近の並び・検索・ピン留め / サムネイルの切り出し・縮小 / ニュース本文の整形） |
| `src/project/LauncherMotion.h` | 純ロジック（指数補間 / 臨界減衰スプリング / 入場スタッガー / 背景の粒） |
| `src/project/ProjectManager.{h,cpp}` | データ層（recent.json / editor_state.json / OS のダイアログ）。`DX12E_DATA_DIR` で保存先を差し替え可 |
| `src/core/ApplicationRender.cpp` | ランチャーの呼び出し（画像ローダーを渡す）と、閉じる直前のサムネイル要求 |
| `src/core/ApplicationProject.cpp` | サムネイルの撮影・保存（`UpdateProjectThumbnail`） |
| `tests/launcher_logic_test.cpp` | 純ロジックの単体テスト（`LauncherTests`） |
| `src/gui/UiTestHarness.cpp` の `T_LauncherScreen` | 実画面の UI 自動テスト（タブ・検証表示・テンプレ選択・検索） |
| `tools/launcher_art/` | テンプレ画像 / ヒーロー背景をエンジンで描く手順（下記） |

## 画面構成

- **ナビ**（左 256px）: 最近 / 新規 / 開く / Git からクローン / ニュースと学習。選択帯はスプリングで滑る。下部に GitHub のログイン状態と「背景アニメーション」スイッチ、上部にロゴとバージョン。
- **最近**: 検索欄（Ctrl+F）+ カードのグリッド。カードは 16:9 のサムネイル・名前・最終オープン（相対時刻）・パス（中央省略）・ピン。先頭に点線の「新規プロジェクト」タイル。存在しないパスは警告表示（開けない）。ダブルクリック / Enter で開く、右クリックでメニュー（開く / ピン留め / エクスプローラーで表示 / パスをコピー / 一覧から削除）。右の詳細パネルにサムネイル・パス・最終オープン・エンジン版・開始シーン、下に「開く」「ピン留め」「一覧から削除」（インライン確認）。
- **新規**: テンプレートのギャラリー（4 枚）+ アプリ内フォーム（プロジェクト名 / 保存場所 + 「参照…」/ 作成先プレビュー / 検証メッセージ / 影の品質 / VSync / Git 初期化）。右の詳細パネルにテンプレの大きなプレビュー・説明・含まれる機能・同梱物。
- **開く**: パスを入れて（またはダイアログで .dx12proj を選んで）開く。フォルダ指定・引用符つきのパスも可。
- **クローン**: URL と保存先を入れてクローン。ワーカースレッドで走り、画面は止まらない。
- **ニュース**: 更新内容（`core/ReleaseNotesData.inc` の最新版を、注目と種類別の節に整形）/ バージョン情報 / 学習リンク（ドキュメント・Lua ガイド・MCP・サンプルとテンプレート・GitHub・リリースノート）。

## データモデル

- `launcher::RecentRecord { name, path, lastOpened(epoch秒), pinned }` — `recent.json` は `{"recents":[{"name","path","lastOpened","pinned"}]}`（旧形式の name/path だけも読める）。並びは「ピン留め → 新しい順」。ピン無しは 24 件まで（ピン留めは落とさない）。
- 画面内の `RecentView` は上に「存在するか（フォルダ + .dx12proj）」「サムネイルの有無・テクスチャ」「文字の省略キャッシュ」を持つ。
- `launcher::TemplateDef` — テンプレ登録表の 1 行（id / 名前 / ジャンル / 1 行説明 / 説明 / 機能チップ / 同梱物 / カード画像 / アイコン / アクセント色 / フォールバックのグラデ）。

## テンプレートの増やし方

1. `src/project/ProjectTemplates.cpp` の `GetFiles()` に ID とファイル表を足す（`assets/scenes/main.json` を必ず含める）。
2. `src/project/LauncherLogic.h` の `Templates()` に 1 行足す。画像が無ければ `cardImage` は `""` でよい（グラデ + アイコンで表示される）。
3. 画像は下の手順で撮って `assets/editor/launcher/` に置く。

`LauncherTests` が「ID の一意性 / `GetFiles` に実体があり `main.json` を含む / 指定した画像ファイルの実在 / 説明文に方言が無い」を検査する。

## サムネイル

- 保存先: `<プロジェクト>/.dx12/thumbnail.png`（640x360・PNG）。`.dx12/` は既にセーブやバックアップを置くユーザー領域で、`GitIntegration::WriteGitignore` が `.dx12/` を除外する。
- 撮るタイミング: ①ファイルメニュー等でプロジェクトを閉じるとき（必ず）②保存したとき（前回から 45 秒以上あいた 1 回だけ）③プロジェクトを開いて 6 秒後にまだ無いとき（1 回だけ）。Play 中は撮らない。
- 中身: 直近のシーン描画（`m_sceneRT` をポスト相当の表示変換で読み戻し）→ 中央 16:9 を切り出し → 箱フィルタで縮小。真っ黒 / 単色（暗転中など）は保存せず、前の良い画を残す。
- 読み込み: ホストが「更新日時 + サイズ」をキーにテクスチャ化する。更新されたら別物として読み直される。

## 検証ルール一覧（`launcher::ValidateNewProject`）

| コード | 重さ | 内容 |
|---|---|---|
| `name.empty` / `name.edge` / `name.chars` / `name.reserved` / `name.too_long` | エラー | 空 / 先頭末尾の空白・ピリオド / `<>:"/\|?*` と制御文字 / CON・PRN・AUX・NUL・COM1-9・LPT1-9 / 120 バイト超 |
| `name.long` / `name.dot_start` / `name.fullwidth_space` / `name.non_ascii` | 警告 | 64 文字超 / 先頭ピリオド / 全角スペース / 日本語などの非 ASCII |
| `loc.empty` / `loc.relative` / `loc.chars` / `loc.segment_edge` | エラー | 空 / ドライブ付きのフルパスでない / パスに禁則文字 / フォルダ名の先頭末尾が空白・ピリオド |
| `loc.not_dir` / `loc.no_root` / `loc.not_writable` | エラー | 保存場所がファイル / ドライブや上位が無い / 書き込めない（一時ファイルを作って確かめる） |
| `target.is_file` / `target.exists_nonempty` / `target.already_project` | エラー | 作成先に同名のファイル / 中身のあるフォルダ / すでにプロジェクト |
| `path.too_long` | エラー | 作成先が 240 バイト超（Windows の 260 文字上限を割る） |
| `path.non_ascii` / `path.fullwidth_space` / `path.long` / `loc.protected` / `loc.onedrive` / `loc.inside_project` / `dup.recent_name` | 警告 | 保存場所の日本語 / 全角スペース / 180 バイト超 / Program Files・Windows 配下 / OneDrive 配下 / 別プロジェクトの中 / 最近の一覧に同名（別の場所） |
| `loc.will_create_parents` / `target.exists_empty` | 情報 | 保存場所のフォルダを作る / 同名の空フォルダの中へ作る |

Git クローンは `ValidateGitUrl`（`url.empty` / `url.space` / `url.scheme` / `url.no_repo`）+ `ValidateLocation(forClone)`。
検証は入力が 0.18 秒止まってから走る（ディスクを叩くため）。走るまで「作成」は押せない。

## テンプレ画像 / ヒーロー背景の作り方（エンジン内レンダリング）

画像は `assets/editor/launcher/` の `hero.png`（1600x900）と `tmpl_fps|tps|2d|empty.png`（800x500）。
すべてこのエンジンでプリミティブ（box / sphere / plane）とライト・ポストだけで組んだシーンを描いたもので、外部素材は使っていない。

```powershell
# 1. 撮影用シーンを使い捨てプロジェクトへ書き出す（assets/scenes/launcher_*.json）
python tools/launcher_art/build_scenes.py --out $env:TEMP\launcher_art_proj

# 2. エンジンを --background・高倍率で起動（--project は付けない。人の画面/カーソルには触れない）
cd build\release
Start-Process .\DX12Engine.exe -ArgumentList '--background','--dpi-scale','2.0','--mcp-port','8852' -WindowStyle Hidden

# 3. MCP で開いて Play → screenshot_final → Stop（Play 中は MainCamera の画角でポスト適用後の絵になる）
node tools/launcher_art/render.mjs --port 8852 --project $env:TEMP\launcher_art_proj --out $env:TEMP\launcher_art_raw

# 4. 大きさを整えて assets/editor/launcher/ へ
python tools/launcher_art/finalize.py --raw $env:TEMP\launcher_art_raw
```

構図・ライトの調整は `build_scenes.py` の各 `build_*()` を直して 1〜4 をやり直す（`--only fps,hero` で一部だけ）。`--dpi-scale 2.0` は撮影解像度を上げるため。Pillow が要る。
画像が無い / 読めないときは、画面側がグラデーション + アイコンにフォールバックする。

## 設定（アニメーション）

`editor_state.json`（`%APPDATA%\DX12Engine\`）の `launcherAnimations`。左下の「背景アニメーション」スイッチで切り替える。
初期値は Windows の「アニメーション効果」設定に従う（切っていれば OFF）。OFF のときは、背景の光・粒・視差・タブの入場・カードの登場・ホバーの補間がすべて止まり、目標値へ即座に切り替わる。
その他の保存項目: `launcherLastLocation`（前回の保存場所）/ `launcherLastTemplate`。

## 検証用の環境変数

`DX12E_DATA_DIR=<フォルダ>` … recent.json / editor_state.json の場所を差し替える（実ユーザーの一覧を触らずに画面を試せる）。

## 動きの仕様（linear は使わない）

| 動き | 方式 |
|---|---|
| ホバー / 選択 / ボタン | 指数補間（半減期 50〜80ms。フレームレート非依存） |
| ナビの選択帯 | 臨界減衰スプリング（ω=22。行き過ぎない） |
| タブ切替 | フェード 0.26s（out cubic）+ 詳細パネルが右からスライド 0.34s |
| カードの登場 | 1 枚ずつ 45ms ずらして下から 0.36s（out cubic） |
| 背景 | ゆっくり漂う光 3 つ + 粒 46 個 + マウスの視差（±10px）。すべて ImDrawList |

## 罠

- `recent.json` / `editor_state.json` の読み書きはロードスレッドとメインの両方から来る。`ProjectManager` の 1 本のミューテックスで守っている。
- `std::filesystem` に UTF-8 の `std::string` を直接渡すと ANSI 解釈になって日本語のフォルダが壊れる。`launcher::PathFromUtf8` を必ず通す（`Project.cpp` も直した）。
- `PathResolver::AssetsDir()` はプロジェクトを開くとそのプロジェクトの assets/ に変わる。ランチャーの画像は exe の位置から引いたエンジン組み込みの assets/ を使う（プロジェクトから戻った後も出るように）。
- `small` は Windows のマクロ。変数名に使えない。
- ImDrawList の同心円の重ねでグローを作ると輪が見える。`GlowCircle` は頂点色の補間で描く。
- 子ウィンドウ（スクロール領域）の中では `ImGui::GetWindowDrawList()` を取り直すこと。親の drawlist に描くと子の背後に隠れる。
