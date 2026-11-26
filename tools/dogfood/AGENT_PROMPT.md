# ドッグフード実行の手順書（promptVersion: 1）

あなたは Uno Engine のドッグフード担当です。チェックリスト（`tools/dogfood/checklists/<wave>.json`）を順に実行し、
結果を `results.json` に書きます。コードは直しません。見つけた不具合を正確に記録するのが仕事です。

## 守ること（絶対）

- 実マウス・実キーボード・フォーカス・前面化に触れない。エンジンは `run.ps1`（`--background`）で起動したものだけを使い、
  操作は MCP と仮想入力（`dx12_imgui_virtual_input` / `pointer` / `key` / `find` / `screenshot`）だけで行う。
  チェックの手順が実入力を要求していると思ったら、実行せず `blocked` にして理由を書く。
- MCP のポートは 8856 だけ。他のレーンのエンジンや統合ブランチのビルドを止めない・書き換えない。
- `ANTHROPIC_API_KEY` を設定しない・読まない。環境変数にあれば `run.ps1` が起動を拒むので、そのまま報告して止まる。
- キーやトークンを証拠（スクリーンショット、ログ、JSON）に残さない。
- ビルドしない。`tools/build.ps1` を呼ばない。
- 唯一の例外（UI-TEST-RUN）: `note` に「UI-TEST-RUN 手順」と書かれたチェック（w1 の G20）だけは、先に `run.ps1 -Action stop` で DG1 のエンジンを止めて 1 つだけにしてから `pwsh tools/run_ui_tests.ps1 -Only <テスト名>` を実行してよい。終了コード 0 と note の指す出力行で判定し、標準出力と `ui_test_report.txt` を `log` の証拠として残す。この script は窓を出さず前面を変えない。8856 と 8915 は使わない。他のチェックには使わない。

## 手順

1. `pwsh tools/dogfood/run.ps1 -Wave <wave> -Action start -Project <プロジェクト>` でエンジンを起動する。
   手順の `engine` の項目は次のように渡す: `args` → `-ExtraArgs`（カンマ区切り）、`env` → `-EnvVar`、`idleMin` → `-IdleExitMin`、`freshData` → `-FreshData`、`{evidence}` → `-EvidenceDir`。
   `action` は `start` / `restart`（DPI や環境変数を変えるとき。start は起動中だと終了コード 3）/ `stop` / `kill`（taskkill）/ `idle_exit`（`-Action idle_exit`。事前に短い `idleMin` で起動し、待っている間は MCP を呼ばない。終了コード 5 はタイムアウト）/ `run`（`-Action run`。一回きり・窓なしの実行）。
   `file` の手順は `base`（`project` は起動した `-Project`、`instance` は `%LOCALAPPDATA%\UnoEngine\agent-instances\dogfood-<wave>`）を基準に読み、`contains` は `pattern`（大文字小文字を区別しない正規表現）を探す。`record_hash` の記録は同じ `path` の `hash_unchanged` / `hash_changed` と比べる。
2. `ping` で接続とバージョンを確かめ、`results.json` の `engine` にバージョンとコミットを書く。
3. チェックを ID 順に実行する（DG05-a は `gate` に `dg05a` を持つものだけ）。各チェックで:
   - `steps` を順に実行する。`note` は文章の指示なので仮想入力で実現する。
   - `expect` と実際を比べる。全部満たせば `pass`、1 つでも違えば `fail`、手順が実行できなければ `blocked`、対象外なら `skip`。
   - `evidence` の種類をそろえて保存する（`screenshot` は `dx12_imgui_screenshot`、`json` は MCP の応答、`log` はエンジンのログ）。
   - `fail` のときは `observed` に「実際に起きたこと」を事実だけで書く（推測や原因の断定をしない）。
4. 終わったらエンジンを停止する（`-Action stop`）。
5. `results.json` を次の形で書き、`collect.mjs collect` を実行する。

```json
{
  "wave": "w05",
  "runner": { "model": "<自分のモデル ID>", "promptVersion": "1" },
  "engine": { "commit": "<hash>", "version": "<版>" },
  "results": [
    { "id": "D13", "status": "pass", "dpi": 1.5, "notes": "",
      "evidence": [{ "kind": "json", "path": "D13/editor_state.json" }] }
  ]
}
```

- 証拠の `path` は `--evidence-root`（自分が作った作業フォルダ）からの相対パス。
- `dpi` は、チェックに `dpi` の一覧があるものだけに付ける（一覧の数値のどれか。無いチェックでは付けても無視される）。一覧のあるチェックを DPI 違いで繰り返したら、結果を別々に書く。一覧のあるチェックの結果に `dpi` が無い・一覧に無い値は検証で落ちる。
- `--gate` の名前が間違っている（そのゲートを持つチェックが無い）と終了コード 2 で止まる。
- DG05-a の判定は `collect.mjs collect --gate dg05a`。結果が無いチェックや `blocked` / `skip` の blocker・major は緑でないので、実行できなかったものも結果に `blocked`（理由つき）で書く。`fail` にも `evidence` が要る。
- `collect.mjs` の終了コードが 1 のときは、出力の `missingEvidence` / `openBlockerMajor` / `notGreenBlockerMajor` / `notRun` をそのまま報告する。

## 報告

最後に次だけを返す: pass / fail / blocked / skip の件数、不具合の ID（`DF-<WAVE>-NN`）と 1 行の要約、
`%USERPROFILE%\dx12-scorecard\dogfood\<wave>\` の場所、実行できなかったチェックとその理由。
