# 危険操作と運用ルール
> guarded な操作は確認してから。人の PC 操作を奪わない。破壊的な操作は使い捨てプロジェクトで。

## guarded(`dx12_call` は `confirm:true` が無いと拒否)
`git_commit` / `git_push` / `git_pull` / `git_fetch` / `git_checkout` / `git_merge` / `git_merge_abort` / `eval_lua` / `delete_asset` / `build_game` / `net_launch_test_client`。
手順: ① `dx12_call {name, args, dryRun:true}` で影響を確認 → ② ユーザーの承認を得る → ③ `confirm:true` を付けて撃つ。
**core 面(`DX12_MCP_SURFACE=core`)**: `dx12_call` では `confirm:true` でも通らない(E_GUARDED)。`dx12_call_guarded {name, args}` から実行する(毎回ユーザーが承認する。先に `dryRun:true`)。`dx12_batch` に guarded な op が混じると 1 つも実行せず E_GUARDED。
(旧ツールとして直接呼ぶ場合は、クライアントの権限設定(名前ベースの allow/deny)が効く。)

## dryRun
`dx12_call {dryRun:true}` は副作用のある操作を実行せず、対象の存在・破壊性・Undo 可否を返す。`look_apply` / `vfx_apply` / `decal_apply` / `sequence_author` / `organize_scene` は native dryRun(実際の計算結果を返す)。読み取り系は実行される。

## 未保存の変更を消す操作
`open_scene` / `new_scene` / `open_project` は現在のシーンを閉じる。`sceneDirty:true` のときは meta.warnings に出る。MCP 接続中は自動保存されるが、外部でシーン JSON を書き換えた直後は上書きの競合に注意(書いたら即 open、事前に `sceneDirty:false`)。

## Undo
MCP 呼び出し 1 回 = Undo 1 エントリ「AI: <method>」(Editor モードのみ)。まとまった編集は `dx12_batch`(atomic)か `dx12_transaction_begin` 〜 `commit`。`undo` は既定で AI の分だけ戻す。ファイルを書く操作(`create_lua_component` / `scene_write` / `import_asset` など)は Undo で戻らない(`<project>/.dx12/backups/` に 20 世代)。

## 人の作業を守る
- エンジンは `--background` で起動する(editor ガイド参照)。実マウス/実キーボード/前面化は使わない。
- 起動したエンジンは終わったら閉じる。
- 使い捨てのプロジェクトで試す。人が開いているエディタを奪わない(単一クライアントのブリッジなので、別セッションが握っていると応答が無い → `dx12_doctor` が E_ENGINE_BUSY と診断する)。
