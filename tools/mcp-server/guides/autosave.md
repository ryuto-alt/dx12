# 自動保存と人の未保存の変更(autosave)
> 人の未保存の変更があるとき、AI の作業はディスクに書かれない。結果の autosave.blockedByHumanEdits が true なら、人に保存を依頼する。

## 何が起きるか
- MCP 由来の編集だけなら、自動保存はシーン本体(`scenes/*.json`)へ書く。
- 人の編集(エディタでの操作・仮想入力を含む)が混ざっていると、**シーン本体には書かない**。代わりにシーンの世代(`scene_backups`)へ理由 `protect-human-edits` 付きで退避する。AI の作業はディスクの本体に残らない。
- `save_scene` と `editor_state` の結果に `autosave: { blockedByHumanEdits, lastBackupId }` が付く。`blockedByHumanEdits:true` のときは `warnings[]` に「人の未保存の変更があるため自動保存を止めています。人に保存を依頼してください」が出る。

## AI がやること
1. `dx12_editor_state {scope:"mode"}` か `dx12_save_scene` の結果で `autosave.blockedByHumanEdits` を見る。
2. true なら作業を続ける前に、`dx12_editor_notify` で人へ「保存してください」と伝え、人が保存するまで `open_scene` / `new_scene` を撃たない(AI のモーダル待ちで固まらないよう、撃てば退避してから進むが、人の編集は本体に戻らない)。
3. 退避された編集は `dx12_call {name:"scene_backups", args:{op:"list"}}` → `{op:"restore", id:lastBackupId}` で戻せる(先に `dryRun:true`)。
4. 人が保存した後は、次の MCP の編集から再び本体へ書かれる。

## 終了の経路(MCP に終了メソッドは無い)
- **アイドル終了**(fleet のインスタンスが一定時間使われず閉じるとき): 人の未保存の変更があれば本体に書かず、上と同じく世代へ退避してから終わる。人の編集が無ければ従来どおり本保存する。
- **taskkill / 強制終了**: 確認も保存も通らない。最後の砦は既存の自動保存の退避(世代)だけ。人の編集が本体に残る保証は無いので、重要な作業の前に人へ保存を依頼する。
- **ウィンドウを閉じる**(人の操作): 未保存の確認が出る。AI から閉じる手段は無い。
