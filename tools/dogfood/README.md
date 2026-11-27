# ドッグフードの道具

Wave ごとのゲート（W0.5 の DG05、W1 の DG1）で、エンジンを「人が使うように」動かして不具合を拾うための道具。
実行者は Claude Code のサブエージェント 1 体（サブスクリプション。有料の API は使わない）。

| ファイル | 役割 |
|---|---|
| `checklists/w05.json` | W0.5 のチェック D1, D1b, D2〜D16（`gate: ["dg05a"]` が DG05-a の組） |
| `checklists/w1.json` | W1 のチェック G1〜G20 |
| `AGENT_PROMPT.md` | サブエージェントへの手順書（版は先頭の `promptVersion`） |
| `run.ps1` | エンジンの背景起動・再起動・停止・kill・アイドル終了待ち・一回きりの実行（MCP 8856 / UI 8915 専用） |
| `collect.mjs` | チェックリストと結果の検証、証拠の収集、不具合 `DF-<wave>-NN` の採番 |
| `bug_schema.json` | チェックリスト・結果・不具合の形（仕様の正） |
| `collect.test.mjs` | 単体テスト（`npm test` の一覧に入っている） |

## チェックの形

`{id, area, target, title, steps[], expect, evidence[screenshot|json|log], severityIfFail, gate?, dpi?}`。

手順の種類は次の 7 つだけ。実マウス・実キーボード・フォーカス・前面化を要求する種類は無い。

- `mcp`: MCP の呼び出し（`method`, `params`）
- `input`: 仮想入力（`virtual_input` / `find` / `pointer` / `key` / `screenshot` = `dx12_imgui_*`）
- `wait`: 待ち（ミリ秒）
- `file`: ファイルの確認（`exists` / `absent` / `record_hash` / `hash_unchanged` / `hash_changed` / `contains`。読むだけ）。`path` の基準は `base`（`project` = プロジェクトの根が既定。シーンは `assets/scenes/...`、`instance` = エンジンのインスタンスフォルダ。ログは `dx12_engine.log`）。`contains` は `pattern`（大文字小文字を区別しない正規表現）が要る。`hash_*` は同じチェックの中で先に同じ `path` を `record_hash` していること（検証器が見る。`*` を含む path は一致した全ファイルのハッシュの組）
- `engine`: エンジンの操作（`run.ps1` 経由）。`action` は `start` / `restart` / `stop` / `kill` / `idle_exit`（短い `idleMin` で起動しておき、MCP を呼ばずに自己終了を待つ）/ `run`（一回きり・窓なし。`--splash-selftest <dir>` の形）。任意: `args`（エンジンの引数。`{evidence}` は証拠フォルダに置き換わる）、`env`（`NAME=VALUE` の配列。`-EnvVar`）、`idleMin`（`--idle-exit` の分。小数可）、`freshData`（起動前にインスタンスの data を消す。start / restart だけ）
- `screenshot`: 撮影の名前の指定
- `note`: 文章での指示（仮想入力で行う操作の説明など）。例外: 「UI-TEST-RUN 手順」と書かれた note（w1 の G20）は、エンジンを止めてから `tools/run_ui_tests.ps1 -Only <テスト名>` を実行する指示で、AGENT_PROMPT.md がこれだけを許している

## 使い方

```
# 1. エンジンを背景起動（窓は出ない。ANTHROPIC_API_KEY があれば拒む）
pwsh tools/dogfood/run.ps1 -Wave w05 -Action start -Project <プロジェクト> [-Dpi 1.5] [-EnvVar DX12_D3D_DEBUG=1] [-IdleExitMin 1] [-FreshData]
# 2. サブエージェントが AGENT_PROMPT.md に従ってチェックを実行し、results.json を書く
# 3. 検証と収集
node tools/dogfood/collect.mjs collect --wave w05 --results <results.json> --evidence-root <証拠の置き場> [--gate dg05a]
# 4. 停止
pwsh tools/dogfood/run.ps1 -Wave w05 -Action stop
```

- 出力: `%USERPROFILE%\dx12-scorecard\dogfood\<wave>\{results.json, bugs.json, shots\, logs\, json\}`。
- 再実行（失敗したチェックと同じ領域だけ）は同じ出力先へ。結果は ID と DPI の単位で差し替わり、
  直ったチェックの不具合（open でも、レーンが `fixed` にしたものでも）は `verified` になる。`fixed` のあとにまた落ちたら同じ番号を開き直す（`reopenedAt`）。`verified` のあとの再発は新しい番号になる。
- 終了コード 1: (1) blocker / major の open の不具合が残る (2) 証拠が足りない（pass と fail の両方で必要な種類を要求する）(3) 対象の blocker / major が緑でない（結果が無い・blocked・skip・fail）。`--gate dg05a` で対象を DG05-a の組に絞れる（省略で全チェック。そのゲートを持つチェックが無い名前は終了コード 2）。`dpi` の一覧を持たないチェックの結果の `dpi` は無視し、持つチェックでは一覧の数値でなければ検証で落ちる。minor の未実施は `notGreenMinor` に出すだけで終了コードには効かない。
- 不具合は ID + DPI の単位。ある DPI の fail は、その DPI の pass でだけ verified になる。
- 要約は pass / fail / blocked / skip を分けて数え、`notRun`（結果が無いチェック）も出す。
- チェックリストだけの検査: `node tools/dogfood/collect.mjs validate-checklist`。

## 規則

- blocker と major は次のマージ（W0.5 は W1 の IF チャンク、W1 は W1 のタグ）の前に直す。minor は次の Wave の磨き込みへ。
- エンジンは 1 台。オーナーの離席中に回す。実マウスとフォーカスは動かさない。
- 証拠は `--evidence-root` の外のファイルをコピーしない。スクリーンショットや JSON に鍵やトークンを写さない。
