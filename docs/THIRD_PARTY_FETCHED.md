# 取得して使う依存（リポジトリにコミットしないもの）

このリポジトリは MIT で公開している。再頒布ライセンスが MIT と相容れない、または大きなバイナリである依存は、
**コミットせず、スクリプトで取得して使う**。ここはその一覧と、例外を認めた根拠の記録。

| 依存 | 取得 | 置き場 | ライセンス文の同梱先 | 決定 |
|---|---|---|---|---|
| D3D12 Agility SDK のランタイム（`D3D12Core.dll` / `d3d12SDKLayers.dll`） | cmake の configure が自動（`cmake/UnoAgility.cmake`）。手動は `pwsh tools/fetch_agility.ps1` | `third_party/agility/<版>/`（gitignore） | インストーラの `{app}\licenses`、更新用 zip の `licenses\` | 2026-11-26 オーナー決定 1（Agility SDK を採用） |
| NVIDIA DLSS SDK（NGX） | `pwsh tools/fetch_dlss.ps1` | `third_party/dlss/`（gitignore） | `third_party/dlss/LICENSE.txt` | 2026-11-26 オーナー決定 2（DLSS は任意の依存） |

## D3D12 Agility SDK

- 版・SDK 番号・SHA256 の正は **`cmake/agility.lock.json` の 1 つ**。他の場所に数字を書かない
  （C++ は lock から生成する `UnoAgilityLock.h`、`D3D12_SDK_VERSION` との `static_assert` で合わせる）。
- 固定版は retail の **1.620.x**。プレビュー版（SDK 番号 700 以上）は出荷しない（ctest `AgilityVersionPolicy`）。
  上げるのは Wave の境目の IF チャンクだけで、全ゴールデンと性能 A/B を取り直す。
- ヘッダは vcpkg の `directx-headers`（`vcpkg.json` の `overrides` で固定。`builtin-baseline` は据え置き）。
  ランタイムの DLL だけを NuGet `Microsoft.Direct3D.D3D12` から取る。取得は `file(DOWNLOAD ... EXPECTED_HASH)` と
  `file(LOCK)` で直列化し、一時フォルダへ展開してから rename する（複数の worktree が同時に configure しても競合しない）。
- 配置: exe の隣の `D3D12\`（`D3D12SDKPath = ".\D3D12\"` は exe からの相対）。ビルドツリーには `d3d12SDKLayers.dll` も置く
  （Release でも `DX12_D3D_DEBUG=1` でデバッグレイヤーを使うため）。配布の 3 経路（ゲーム配布 / インストーラ / 更新用 zip）は
  `D3D12Core.dll` だけを運び、SDKLayers は含めない。
- 再頒布ライセンス（`Microsoft.Direct3D.D3D12` の `LICENSE.txt`）に従い、ライセンス文を配布物に同梱する（ゲーム配布は `licenses\Microsoft.Direct3D.D3D12-LICENSE.txt`。ビルドツリーの `D3D12\LICENSE.txt` から運ぶ）。
- エンジンを別の場所へコピーして起動するときは `D3D12\` も運ぶ（`tools/engine_instance.ps1` は対応済み）。

## vcpkg の版の上書き（overrides）と worktree

`vcpkg.json` の `overrides`（directx-headers 1.620.2 / d3d12-memory-allocator 3.3.0）は、`VCPKG_MANIFEST_INSTALL=ON` で configure したときだけ効く。
従来の `tools/worktree_setup.ps1` が作るレーンは、本体の `build\release\vcpkg_installed` を共有して `VCPKG_MANIFEST_INSTALL=OFF` で configure するため、
そのままでは古い版（1.619.1 / 3.0.1）が使われ、`AgilityExports.cpp` の `static_assert`（D3D12_SDK_VERSION == lock の SDK）でビルドが止まる。

`worktree_setup.ps1` は、本体の installed dir に overrides の版が無いとき（または `-OwnDeps` を付けたとき）、
本体の installed dir を複製したレーン専用の dir（`build\release\vcpkg_installed`）で `MANIFEST_INSTALL=ON` にして configure する（新しいレーンに手作業は要らない）。
本体の installed dir が overrides の版になっていれば、従来どおり共有（OFF）のままにする。

既に configure 済みのレーンを手で切り替えるとき（`worktree_setup.ps1 -Remove` して作り直すのが確実）:
1. レーン専用の installed dir を作る（本体の `vcpkg_installed` を robocopy で複製）。
2. `cmake -S . -B build\release -DVCPKG_INSTALLED_DIR=<レーンの build\release\vcpkg_installed> -DVCPKG_MANIFEST_INSTALL=ON`（vcvars64 と VS の Installer フォルダ（vswhere）が PATH にある状態で）。
3. `CMakeCache.txt` に残る本体側の `vcpkg_installed` への絶対パス（`*_DIR`、`ZLIB_*` など）を、レーンのパスへ置換する。

M2 の夕方（メイン）: 本体を `-DVCPKG_MANIFEST_INSTALL=ON` で 1 回 configure して共有の installed dir を 1.620.2 / 3.3.0 にし、
各レーンを rebase する。その後に作るレーンは自動で共有（OFF）になる。

## NVIDIA DLSS SDK（NGX）

- 取得: `pwsh tools/fetch_dlss.ps1`。版の正は同スクリプトの `$Tag` と `$ExpectedCommit`（注釈付きタグが指すコミット）の 2 つで、
  取得後に `git rev-parse HEAD` が `$ExpectedCommit` と一致しなければ終了コード 4 で失敗する。DLL の実体は git-lfs が pointer の sha256 で照合する。
  版を上げるときは 2 つを一緒に変える（`git ls-remote https://github.com/NVIDIA/DLSS.git "refs/tags/<tag>^{}"`）。
- コミットしない: `third_party/dlss/` は `.gitignore` 済み。SDK の中身を git に入れる経路は無い（NVIDIA RTX SDK ライセンス 4.e）。
- ビルド: `pwsh tools/build.ps1 -Dlss auto|require|off`。
  - `auto`（既定）: SDK があれば DLSS 付き、無ければビルドの最後に枠付きで `DLSS: OFF (pwsh tools/fetch_dlss.ps1)` を出して DLSS 無しで成功する。
  - `require`: SDK が無ければビルドせず終了コード 3。夜間ビルド（性能の合否は DLSS 列）と公式リリースで使う。
  - `off`: `-DUNO_ENABLE_DLSS=OFF`。出力に古い `nvngx_dlss.dll` が残っていれば消す。
  - ゴールデンとショーケースは決定論モードで常に DLSS OFF なので、どのモードでも結果は同じ。
  - DLSS の有無が前回の configure と違うときだけ、`build.ps1` が configure し直す（`<ビルドdir>/.uno_dlss_configured` に記録）。
- 配布: `nvngx_dlss.dll` は必ず NVIDIA のライセンス文とセットで配る（`licenses\NVIDIA_DLSS_LICENSE.txt`）。
  - ビルドツリー: CMake が `nvngx_dlss.dll` と `licenses/NVIDIA_DLSS_LICENSE.txt` を exe の隣へ置く。
  - ゲーム配布（`ApplicationBuild.cpp`）: DLL を同梱するときだけライセンス文もコピーする（ライセンス文が無ければ DLL を同梱しない）。
  - インストーラ / 更新用 zip（`installer/build.ps1 [-Dlss auto|require|off]`）: 既定 `auto` は DLL が無ければ警告つきで DLSS 無しの配布物を作る。
    公式リリースは `-Dlss require`（DLL かライセンス文が無ければ失敗）。
- 検査: ctest `DlssWiringCheck`（`DlssBackend::CompiledIn()` と exe の隣の `nvngx_dlss.dll` の有無の一致、`installer/build.ps1` の auto / require の判定）。
- 実行時: DLSS が使えない環境では `DlssBackend` が Warn を 1 回だけログに出し、TAA で描く（`get_upscaler` の `fallbackReason` に理由が入る）。
