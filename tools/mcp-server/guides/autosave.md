# 自動保存と人の未保存の変更(autosave)
> 人の未保存の変更があるとき、自動保存はシーン本体(`scenes/*.json`)へ書かない。結果の autosave.blockedByHumanEdits が true なら、人に保存を依頼する。

## 何が起きるか
- MCP 由来の編集だけなら、自動保存はシーン本体へ書く。
- 人の編集(エディタでの操作・仮想入力を含む)が混ざっていると、**自動保存はシーン本体に書かない**。代わりにシーンの世代(`scene_backups`)へ理由 `protect-human-edits` 付きで退避する(退避は 30 秒間隔に間引かれる)。その間の AI の作業は、人が保存するまでディスクの本体に残らない。
- `save_scene` / `editor_state` / `scene_backups` の結果に `autosave: { blockedByHumanEdits, lastBackupId }` が付く。`blockedByHumanEdits:true` のときは `warnings[]` に「人に保存を依頼してください」が出る。

## save_scene は止まらない(重要)
- 明示の `save_scene` は「自動保存の停止」の対象外で、**人の未保存の変更も含めてシーン本体へ書く**。このとき結果に `autosave.wroteHumanEdits:true` と警告が付く。保存に成功すると人の未保存の印は消えるので、`save_scene` の結果で `blockedByHumanEdits` が true になることはほぼ無い。見るべきは `wroteHumanEdits`。
- 方針: **止まっている自動保存を回避する目的で `save_scene` を撃たない**(人の編集を無断で確定させ、上書きしてしまう)。人に保存を依頼する。やむを得ず撃ったら、`wroteHumanEdits:true` を人に伝える。直前の状態は `scene_backups` で戻せる。

## AI がやること
1. `dx12_editor_state {scope:"mode"}` の結果で `autosave.blockedByHumanEdits` を見る(作業の区切りと、止める・再起動する前)。
2. true なら続ける前に `dx12_editor_notify` で人へ「保存してください」と伝え、人が保存するまで `open_scene` / `new_scene` / `engine_stop` / `engine_refresh` を撃たない。
3. 退避された編集は `dx12_call {name:"scene_backups", args:{op:"list"}}` → `{op:"restore", id:lastBackupId}` で戻せる(先に `dryRun:true`)。
4. 人が保存した後は、次の MCP の編集から再び本体へ書かれる。

## 終了の経路
- **`dx12_engine_stop` / `dx12_engine_refresh` / MCP 側のアイドル停止**: どれも `taskkill /PID /T /F`(プロセスツリーの強制終了)で、確認も保存も通らず、エンジン側のアイドル終了処理も走らない。本体に残るのは終了前の自動保存の結果だけ。人の編集は、30 秒間隔の退避が間に合っていれば `scene_backups` に残るが保証は無い。`engine_refresh` で開き直されるのは本体のシーンで、人の未保存の変更は含まれない。だから**上の手順 1〜2 を先に行う**。
- **エンジン側のアイドル終了**(fleet のアイドル終了。AI の操作歴があり未保存のときだけ動く): 人の未保存の変更があれば本体に書かず、世代へ退避してから終わる。退避に失敗したときは復旧用の自動保存(本体ではない)へ書く。人の編集が無ければ従来どおり本保存する。
- **ウィンドウを閉じる**(人の操作): 未保存の確認が出る。AI から閉じる手段は無い。
