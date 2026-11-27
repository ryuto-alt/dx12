# シーケンサー(.dxseq)を作る・確認する・再生する
> 時間軸の演出は `dx12_sequence {op}` 1 本。まず `load` → `edit` → `scrub` / `eval` で確かめ → `save` → `play` / `autoplay`。

`.dxseq` は `assets/sequences/<名前>.dxseq`(JSON。時刻はティック = 6000/秒、または秒 / フレームで読む op もある)。旧 `dx12_sequence_author`(台本 → Lua)とは別物。形式は `docs/DXSEQ_FORMAT.md`。

## op の一覧
| op | 用途 |
|---|---|
| `list` | シーケンスの一覧 |
| `load` | メモリへ読む。`create:true` で空を作る(ファイルは `save` まで作らない) |
| `get` | 中身(`detail:"full"` で正準形の全文) |
| `edit` | `ops:[SeqOp…]` で編集。1 回 = Undo 1 ステップ(全部成功か全部巻き戻し)。`undo:true` / `redo:true` |
| `eval` | 非破壊で時刻 `t` の値を読む |
| `scrub` | Editor 上に時刻 `t` を適用(見た目を撮れる)。終えるのは `end:true` |
| `save` | 保存。`path` で別名 |
| `play` / `stop` | 再生 / 停止 |
| `autoplay` | Play 開始時に自動再生する設定(`action: list/set/add/remove/clear`) |

## 手順
```
dx12_sequence {op:"load", name:"Intro", create:true, fps:30}
dx12_sequence {op:"edit", name:"Intro", label:"カメラ", ops:[{op:"addBinding", binding:{id:"b_cam", entity:"Camera", tracks:[{id:"t_tr", type:"transform", channels:{"position.x":{keys:[[0,0,"a"],[30000,10,"a"]]}}}]}}]}
dx12_sequence {op:"eval", name:"Intro", t:2.5}                 # 値だけ確認(何も変わらない)
dx12_sequence {op:"scrub", name:"Intro", t:2.5}               # 見た目を 2.5 秒にする
dx12_capture {view:"game"}                                      # 撮る
dx12_sequence {op:"scrub", end:true}                           # 元の値へ戻す
dx12_sequence {op:"save", name:"Intro"}
```
- 編集前に `dryRun:true`(edit / save / scrub / play)で影響だけ見られる。`addBinding` は `entity:"名前"` で対象を指せる。ID は省略可。
- 編集の名前とフィールドは `docs/DXSEQ_FORMAT.md` の SeqOp の節(推測で書かない。エラーの `fix` に直し方が出る)。

## 再生
```
dx12_sequence {op:"play", name:"Intro"}                        # loop: once|loop|pingpong、rate、from、delay、clock:"real"|"game"
dx12_sequence {op:"stop"}
dx12_sequence {op:"autoplay", action:"add", sequence:"Intro"}   # Play 開始時に自動で流す
```
- `scrub` は **Editor 限定**。Play 中は `E_MODE_CONFLICT` になり、`fix` が `op:"play"` を案内する(Play 中は実時間で流れ、イベントも発火する)。
- `play` の既定は実時間(`clock:"real"`)。スローモの影響を受けたいなら `clock:"game"`。

## VFX と一緒に動かす
1. `dx12_vfx_place` で `.uvfx` を置く(`dx12_guide {topic:"vfx"}`)。置いたエンティティは名前で `addBinding` できる。
2. 決定論で撮るなら `dx12_sequence {op:"scrub", t}` のあと `dx12_vfx_seek {t}` を同じ時刻に合わせる(シーケンスの時刻と VFX の時刻は別の時計)。

## つまずき
- シーケンスが無いと言われたら → `op:"list"` で名前を確認、無ければ `load {create:true}`。
- 編集が保存されていない → `save` するまでメモリ上(Editor を閉じると失う)。
- 古い台本方式 → `dx12_tool_search {query:"sequence_author"}`(Lua を生成する別系統)。
