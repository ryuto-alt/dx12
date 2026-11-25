# 並列作業(worktree レーン)運用

大改修(新アイデンティティへ移行中)は、Wave ごとに最大 3 本のレーン(A/B/C)を git worktree で並走させる。
計画の全体は大改修ロードマップ(§4 並列運用モデル)を参照。

## 1. worktree の作り方

```powershell
pwsh tools/worktree_setup.ps1 -Lane A      # C:\Users\ryuto\dx12-wt\lane-a / ブランチ wave-lane-a
pwsh tools/worktree_setup.ps1 -Lane B -Branch wave1/lane-b -Base overhaul/wave0
pwsh tools/worktree_setup.ps1 -Path C:\Users\ryuto\dx12-wt\wt-foo -Branch wt-foo   # レーン外・使い捨て
pwsh tools/worktree_setup.ps1 -Path C:\Users\ryuto\dx12-wt\wt-foo -Remove           # 片付け
```

スクリプトがやること:

1. `git worktree add`(ブランチが無ければ `-Base` から作る。既定は本体 worktree の HEAD で、実行元 worktree の HEAD ではない)。
2. 本体から未追跡の必須物をコピー: `src/core/generated/AssetKey.h`(gitignore 対象)、`third_party/gh-cli`(約 40MB)。
3. `chcp 65001` の下で configure(Ninja / Release)。次の 3 つを本体と共有し、vcpkg の再インストールと assimp の再取得を避ける。
   - `-DVCPKG_INSTALLED_DIR=<本体>\build\release\vcpkg_installed`(約 1.3GB を共有)
   - `-DVCPKG_MANIFEST_INSTALL=OFF`
   - `-DFETCHCONTENT_SOURCE_DIR_ASSIMP=<本体>\build\release\_deps\assimp-src`

前提: 本体の `build\release` が一度ビルド済みであること(vcpkg_installed と assimp-src を参照する)。
本体の vcpkg_installed を作り直す(vcpkg の更新など)ときは、全 worktree のビルドを止めてから行う。
注意: worktree は本体の vcpkg_installed を `VCPKG_MANIFEST_INSTALL=OFF` で共有する。レーンで vcpkg.json に依存を足しても worktree 側には入らない(本体で追加・再インストールしてから各 worktree を再 configure する)。逆に、本体のビルドで manifest install が走ると本体の vcpkg.json に合わせてパッケージが整理され、worktree のビルドに影響しうる。

## 2. ビルド

worktree の中で `pwsh tools/build.ps1`(必ずこれ経由)。ビルドは Mutex(`Global\dx12-build`)で PC 全体 1 本に直列化される。
並走するのはコーディングと検証だけ。初回のフルビルドは約 7 分、worktree 1 本あたりのディスクは約 16GB(フルビルド後)。
ヘッダの変更は各レーンの最初のチャンクにまとめ、再ビルドの連鎖を減らす。

## 3. ポートの帯

| レーン | エンジン MCP | UI テスト |
|---|---|---|
| A | 8851 | 8881 |
| B | 8852 | 8882 |
| C | 8853 | 8883 |
- 8881〜8883 は MCP フリート自動割当帯(8860〜8899、docs/MCP.md)と、MCP の ui_tests ツールが選ぶ帯(8880〜8899)の両方と重なる。フリートのエンジンが同じポートを使っていると衝突するので、その場合は `-Port` で逃がす。非レーンの 8910〜8919 は帯の外。

- 本体(master 側)の UI テスト既定は従来どおり `-Name uitests -Port 8822`。
- `tools/run_ui_tests.ps1` は `-Name` / `-Port` を省略すると worktree 名から決める。
  - フォルダ名が `lane-a` / `lane-b` / `lane-c` で終わる: `-Name uitests-<フォルダ名>`、ポート 8881〜8883。
  - それ以外の worktree: `-Name uitests-<フォルダ名>`、ポート 8910〜8919(フォルダ名から決まる固定値)。
  - 本体: `uitests` / 8822。
- エンジンを手で起動するときも、`--background` か `--headless`(または `tools/engine_instance.ps1`)だけを使い、窓を出さない・マウスを奪わない。

## 4. ルール

- 1 つのファイルの所有者は、Wave の中で 1 レーンだけ。共有の登録表はレーン別のアンカーブロックにだけ追記する。
- マージは Wave の終わりに C → B → A の順で master へ入れ、各マージ後に `build.ps1 -Tests` と各種テストを回す。
- 作業が終わった worktree は `-Remove`(= `git worktree remove --force` + 残ったフォルダの削除)で片付ける。
