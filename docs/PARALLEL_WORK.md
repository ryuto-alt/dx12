# 並列作業(worktree レーン)運用

大改修は、git worktree を使って最大 3 本のビルドレーン(A / BD / C)と、ビルドしないレーン(NB)を並走させる。
運用の要点: 1 つのファイルの所有者は 1 レーンだけにする。共有の登録表は、自分のレーンのアンカーブロックの中にだけ書く。
詳細な所有表と項目は実行パケット(EXEC_W05_W1)の §1.2 / §1.4、背景は docs/EDITOR_IDENTITY.md の「大改修」節を見る。

## 1. レーンとアンカー

| レーン | 役割 | worktree | アンカー | MCP | UI テスト |
|---|---|---|---|---|---|
| A(W0.5 では L1) | 基盤・保存・F0(Agility 等) | `lane-a` | `lane-A` | 8851 | 8911 |
| BD(L2) | シーン・Undo・レジストリ | `lane-b` | `lane-B` | 8852 | 8912 |
| C(L3) | 描画の土台 | `lane-c` | `lane-C` | 8853 | 8913 |
| NB | ビルドしない(別セッション) | `lane-d` | `lane-D` | 8855 | 8914 |

- ドッグフードと MCP-14 の E2E 専用は **MCP 8856 / UI 8915**。レーンには割り当てない。
- 8911〜8915 は MCP フリートの自動割当帯(8860〜8899)の外。
- アンカーの書式は、どのファイルでも次の形に統一する(コメント記号だけがファイルの種類で変わる)。

  ```
  // ==== lane-A begin ====
  // ==== lane-A end ====
  ```

  CMake は `#`、Markdown は `<!-- ==== lane-A begin ==== -->`。レーンは A / B / C / D の 4 つ。
  同じファイルに同じ種類の枠が複数要るときは、名前を付ける: `==== lane-A:decl begin ====`。名前の無い枠の重複は lint が赤にする。
- アンカーがあるファイル: `tests/CMakeLists.txt`、`docs/MCP.md`、`src/core/mcp/ApplicationMcpManifestData.inc`、
  `src/editor/ToolWindows.h`、`src/editor/EditorCommandTable.h`、`src/core/ReleaseNotesData.inc`、
  `src/gui/UiTestHarness.cpp`(kTests)、`src/gui/UiTestHarnessShared.h`(テスト本体の宣言。`lane-X:decl`)。
  新設の `src/renderer/FeatureList.inc`・`src/scripting/LuaBindList.inc`・`src/scene/ComponentSerializerList.inc`・
  `shaders/manifest/lane_{a,b,c,d}.cmake` も同じ書式(中身はアンカーだけ。後続の W05-15 / W05-16 / W05-25 / W05-11 が使う)。
- `ReleaseNotesData.inc` のレーンのブロックは `Release{}` の外の最上位にある。**レーンは自分のブロックに `Release` を足さない**
  (同じ版の `Release` が重なり、版の検査が赤になる)。次の版の `Release{}` はメインが版を切るときに 1 つだけ作る。
  レーンは自分のブロックに、その版に載せる項目の案をコメント(`// W05-xx: ...`)で書き、メインが `highlights` / `items` へ移す。
- **区画の例外**: ルート `CMakeLists.txt` のシェーダ / DXC の区画(346〜456 行と 1556〜1842 行付近)にある旧形式のアンカー 36 行(18 組。`wave<N>-lane<X>` の begin / end)
  は L2 の所有。移行(`shaders/manifest/lane_*.cmake` への置き換え)までは lint の許可リストで通す。
- `src/ecs/Components.h` にはアンカーを置かない(135 TU の再ビルドになるため)。新しいコンポーネントは
  `ComponentSerializerList.inc` と、それを使う TU からの直接の include だけで足す。
- 各 `src/<subdir>/CMakeLists.txt` の末尾に `include(${CMAKE_SOURCE_DIR}/cmake/lanes/<subdir>_lane_{a,b,c,d}.cmake)` の 4 行がある。
  レーンはここへ `target_sources(<target> PRIVATE ...)` で自分のファイルを足す(`OPTIONAL` は付けない)。
  Gui の UI テストは `cmake/lanes/gui_lane_*.cmake` が `src/gui/uitests/lane_*.cpp` を持つ。
- UI テストの足し方: 本体を `src/gui/uitests/lane_<x>.cpp` に書き、宣言を `UiTestHarnessShared.h` の自分の `lane-X:decl` に、
  登録を `UiTestHarness.cpp` の kTests の自分の `lane-X` ブロックに 1 行足す。共有ヘルパ(`uitest::App()` など)の追加は依頼で出す。

## 2. worktree の作り方

```powershell
pwsh tools/worktree_setup.ps1 -Lane A      # %USERPROFILE%\dx12-wt\lane-a
pwsh tools/worktree_setup.ps1 -Lane B -Branch w05/lane-b -Base overhaul/wave0
pwsh tools/worktree_setup.ps1 -Lane D      # ビルドしないレーン。既定で configure しない(-NoBuild)
pwsh tools/worktree_setup.ps1 -Lane D -WhatIf   # 何も作らず、ポート・junction・npm ci の予定だけ表示
pwsh tools/worktree_setup.ps1 -Path %USERPROFILE%\dx12-wt\wt-foo -Branch wt-foo   # レーン外・使い捨て
pwsh tools/worktree_setup.ps1 -Path %USERPROFILE%\dx12-wt\wt-foo -Remove           # 片付け
```

スクリプトがやること:

1. `git worktree add`(ブランチが無ければ `-Base` から作る。既定は本体 worktree の HEAD)。
2. 本体から未追跡の必須物をコピー: `src/core/generated/AssetKey.h`(gitignore 対象)、`third_party/gh-cli`(約 40MB)。
3. 本体に `third_party/dlss` と `third_party/agility` があれば、各 worktree へ **junction** を張る(取得物の二重ダウンロードを避ける)。
   `-Remove` は junction を先に外してから worktree を消す(リンク先の本体側は消さない)。
4. `tools/mcp-server` で `npm ci`(`-NoNpm` で省略)。
5. `VCPKG_DEFAULT_BINARY_CACHE` を共通の固定パス `%USERPROFILE%\dx12-vcpkg-cache` にそろえる。`tools/build.ps1` も同じ既定で設定するので、ビルド中の自動 re-configure にも効く。
6. `chcp 65001` の下で configure(Ninja / Release)。vcpkg_installed と assimp-src は本体と共有する。

前提: 本体の `build\release` が一度ビルド済みであること。
注意: worktree は本体の vcpkg_installed を `VCPKG_MANIFEST_INSTALL=OFF` で共有する。レーンで `vcpkg.json` に依存を足しても
worktree 側には入らない(本体で追加・再インストールしてから各 worktree を再 configure する)。

## 3. ビルドとテスト

- ビルドは必ず `pwsh tools/build.ps1`(PC 全体で Mutex 直列化、BelowNormal)。同時にビルドするのは最大 3 レーン。
- テストは `pwsh tools/ctest.ps1`(VS 同梱の ctest を自動で探し、BelowNormal、`-j3 --timeout 300 --output-on-failure`)。
  - 既定は `-LE "^(gpu|ui)$"`。ラベルは小文字(`gpu` / `ui` / `dxc`)。
  - `-Gpu` で GPU テスト、`-Ui` で UI テスト(エンジンを起動する)を含める。**オーナーの離席中か、メインの指示があるときだけ**。
  - `-R <正規表現>` / `-N`(一覧だけ)。
- デバイスを作るテスト(`D3D12CreateDevice` / `GraphicsDevice` を使う)には必ず `LABELS gpu` を付ける。`GpuLabelLint` が検査する。
- 常に登録される検査(`tests/CMakeLists.txt` の lane-A ブロック。リポジトリのルートは `DX12_REPO_ROOT`。
  CI は `cmake -S tests` で単独構成するので `CMAKE_SOURCE_DIR` を使わない): `GpuLabelLint`、`NoPaidApi`(有料 API の SDK とキーの参照が無いこと。
  `@anthropic-ai` はどこでも不可、`ANTHROPIC_API_KEY` は許可リストの `guard:` に載るファイルの「拒むガード」の行だけ)、
  `AnchorLint`(アンカーの対・4 レーンの存在・重複)、`GenBudgetsCheck`。後の 2 つは node のスクリプト(W05-31 / W05-34)が
  まだ無いうちは SKIP(終了コード 77)になり、スクリプトが入れば再 configure なしで走る。
- UI テストの kTests にレーンが足すテストは `launcher_screen`(プロジェクトを開き直す)と deep 区画より**前**のレーンのブロックに置く。
- エンジンは `--background` / `--headless`(または `tools/engine_instance.ps1`)だけで起動する。窓を出さない・マウスを奪わない。

## 4. マージ順と Wave の境界

- W0.5 のマージ: M0(W05-31 → W05-01 → W05-01b → W05-02a → W05-32)→ M1a → M1b → M2 → M3 → M2b → M4 → M5 → ゲート。
  各マージの後に全レーンが rebase する。受け入れは、その項目と、それより前にマージされた項目だけに依存させる。
- Wave 内のマージは IF(インタフェース変更のチャンク)→ C → BD → A → NB の順。
- ゴールデンの規則: 既存の ID の基準(`tests/golden/baseline/<id>.json` と画像)はレーンで更新・コミットしない。新しい ID は新しいファイルとして
  足すだけ。既存の ID の撮り直しは、メインが承認の後に統合ブランチで行う。
- 生成物(manifest の snapshot、toolSurface の snapshot、budgets の生成物)はレーンでコミットしない。メインがマージ後に作り直す。
- 他のレーンのファイルが要るときは、自分で触らず「依頼: <ファイル> の <区画> に <1 行の内容>(理由)」の書式で出す。

## 5. 重ヘッダ

形(型・メンバ・include)を変えてよい回数が決まっているヘッダ。それ以外の変更が要るときは止めてメインに報告する。

| ヘッダ | 形を変えてよい 1 回 |
|---|---|
| `src/ecs/Components.h`(135 TU) | W0.5 / W1 では変えない。アンカーも置かない |
| `src/graphics/GraphicsDevice.h`(120) | W05-03 の先頭コミット 1 回 |
| `src/graphics/DescriptorHeap.h`(102) | W05-06 の 1 回 |
| `src/editor/EditorTheme.h` / `ThemeVariants.h`(99) | W1 の A2a(IF チャンク) |
| `src/editor/UndoCore.h`(95)、`McpUndoRouter.h`(89)、`UndoSystem.h`(89) | W05-02a(M0) |
| `src/renderer/PostProcessSettings.h`(89) | W05-15 |
| `src/editor/EditorContext.h`(88) | W05-02a(M0)と W1 の A2a |
| `ShadingSettings.h` / `ShadowPcssSettings.h`(80) | W1 の C1(IF) |
| `src/scene/SceneSerializer.h`(67) | W05-11 の 1 回 |
| `src/editor/AssetBrowserLogic.h`(61) | W05-13、W1 の A10 |
| `src/core/Window.h`(60) | 変えない |
| `src/core/SplashScreen.h`(53) | 形は変えない。読む TU を減らすだけ |
| `src/core/Application.h`(51) | W05-01b(M0)の 1 回だけ。以後 W7 まで凍結 |
| `src/renderer/UpscalerMath.h`(51) | W05-08 の 1 回 |
| `src/graphics/GpuTimer.h`(50) | W05-26 の 1 回 |
| `src/editor/LightingPresets.h`(48) | W1 の C10 の 1 回 |
| `src/core/ApplicationInternal.h`(47) | W05-17 が `SplashScreen.h` の include を外すだけ。W1 では凍結 |

## 6. その他

- 作業が終わった worktree は `-Remove`(= junction を外し、`git worktree remove --force`、残ったフォルダの削除)で片付ける。
- リポジトリは PUBLIC。個人のパスやメールアドレスを追跡ファイルに書かない。Agility / DLSS のバイナリはスクリプトで取得し、コミットしない。
