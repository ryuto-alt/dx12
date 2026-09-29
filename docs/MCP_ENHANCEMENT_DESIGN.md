# MCP 強化設計書（AI がゲームを作り・検証し・エディタを操作するための MCP）

- 対象: `tools/mcp-server`（TS サーバ）と `src/core/mcp/*`（エンジン側 TCP ブリッジ）、および `src/editor/EditorCommandTable.h` 系
- 作成: 2026-09-30 / 状態: **調査と設計のみ**（エンジンのソース変更・ビルド・エディタ起動は一切していない。実測は `node index.ts` の stdio 起動（エンジン非接続）と公式ドキュメントの取得のみ）
- 関連: `docs/MCP.md` / `tools/mcp-server/AGENTS.md` / `docs/VIRTUAL_GEOMETRY_DESIGN.md`
- 表記: `path:line` は 2026-09-30 の作業ツリー。**[実測]** はこの調査で計測した値、「公式に記載なし」は憶測で埋めていない項目。

---

## 0. 要約（1 画面）

**現状（実測）**: 公開ツール **220 本**、`tools/list` 応答 **408,644 バイト**（うち全ツール同一で情報の無い `outputSchema` が 44,899 字）。エンジン側 method は **174**。同じ事実が「エンジンの `paramSpec`（型だけ）／TS の手書き zod／`docs/MCP.md`／`AGENTS.md`／README」に**手作業で重複**し、`schemaDrift.ts` がエンジンの `.cpp` を正規表現で読んでドリフトを事後検出している。`instructions` / prompts / resources は無く、`tools/list` を実行時に変える経路が無い。エラーコードは 6 種で「未知の method」も「型違い」も 2 に潰れ、`error_hint` は各ハンドラの手書き。タイムアウトは TS 側だけで、エンジンは処理を続行し、遅れて届いた応答は捨てられ、キャンセルも進捗も無い。

**仕様の事実（出典は §3）**: Claude Code は MCP ツールを**既定で deferred（名前だけ）**にする＝220 本は文脈を食うのではなく**検索精度**の問題。`list_changed` は docs 上は対応だが、**deferred 索引に反映されない/説明が古いまま**という open issue（#66084, #97369）がある。サーバ `instructions` は開始時に必ず載る（2,048 字で切り詰め）。2 分超の呼び出しの自動背景化は**メイン会話だけ**（サブエージェント・非対話は対象外）。外部サーバの `readOnlyHint` を権限判定に使うかは公式に記載なし（権限は名前ベース）。

**推奨案 = 「エンジン登録表（マニフェスト）を唯一の真実にした 3 層のツール面」**（§4）

1. **発見性 (a)**: shell 5 本（常時ロード・定義固定: `dx12_tool_search` / `dx12_tool_describe` / `dx12_call` / `dx12_doctor` / `dx12_guide`）＋ **Core 28 本**（動詞規約・説明テンプレ・consolidated 生成: 描画設定 26→2、地形 12→2、撮影 8→1、imgui 5→1 …）＋長尾は検索して `dx12_call`。**220 → 約 35 本**、`tools/list` を **408 KB → 120 KB 以下**へ。旧 220 名は alias で**恒久サポート**（`DX12_MCP_TOOLSET=full|core|legacy`）。サーバ `instructions` と `dx12_guide` で「いつ何を探すか」を配る。
2. **再起動不要 (b)**: 新 method はエンジンのマニフェスト（`McpDefine` + `McpMeta`、`ping.manifestHash`）から**即 `dx12_tool_describe`/`dx12_call` で使える**（list_changed に依存しない）。TS の複合ツールは `composites/*.ts` をホットリロード。動的登録は補助。
3. **エラー (c)**: 文字列コード体系 + `cause / fix（そのまま撃ち直せる tool+args）/ didYouMean / validValues`、TS とエンジン両方の事前検証、`dryRun`・冪等キー一般化・file journal・`withTransaction`、遅延結果ジャーナル、`dx12_job`（長時間処理）、`dx12_doctor`（未起動・別クライアント占有・版ずれの自己診断）。
4. **①ゲーム制作**: `dx12_apply_scene_spec`（仕様 JSON → 差分計画 → 1 トランザクション生成 → 検証、`specPatch` で自己修正）＋レシピ＋ Brief/監査拡充。**②自動テスト**: ログのリング/カーソル・視覚回帰 baseline・playtest スイート/seed/フレーク・`perf_gate`・UI テスト公開・`ciClient --suite`。**③エディタ**: `EditorCommandTable`（`kCommands` 32 + `window.*` 26 + `create.*` 23 ≒ **81 コマンド**）を `dx12_editor_command` で列挙/実行、`editor_state`（モーダル・ドック・窓）、レイアウト/設定/通知、`imgui_*` は「梯子の最後の手段」＋ OS カーソル不変の証跡つき。**④VG/UE**: `vgeo_info`（TS）・`vg_cook`/`ue_import`（ジョブ）・VG 統計/ストリーミング状態はマニフェスト経由で自動公開・`perf_gate` の `vg_50m` プリセット。VG の P0〜P6・P10 に段階を紐づけ。

**段階（§5、合計 73 実働日）**: M0 プローブ(2) → **M1 マニフェスト+shell(5) → M2 エラー構造化(4) → M3 Core 統合+動的登録(6)** ＝ **17 日で (a)(b)(c) の大半が解消** → M4 文書自動化(3) / M5 安全性(5) / M6 進捗・ジョブ(4) / M7-8 エディタ(9) / M9-10 自動テスト(13) / M11-12 シーン生成(13) / M13-14 VG(9)。M1 の後は 6 レーン並行、M0〜M12 の暦は約 29 実働日。

**撤退条件の要点（§7）**: list_changed が反映されないなら動的登録を切り `dx12_call` に一本化（設計は最初からそれで動く）。ライブ選択率が基準より 5pt 超落ちたら Core を増やし、それでも駄目なら `full` を既定に戻す（旧 220 は無傷）。

**ユーザーに聞くこと（§8）**: ①`core` を既定にする時期と旧名の許可リスト影響 ②`dx12_call` を read/write と guarded の 2 本に分ける権限設計 ③Jev（外部判断 API）を必須依存にしない方針 ④視覚回帰 baseline の承認者と CI の GPU ⑤着手順序（M0〜M3 先行、VG は本体に同期）。

---

## 1. 現行構造の事実（実コード確認）

以降の `path:line` は 2026-09-30 時点の作業ツリー（`C:\Users\ryuto\Documents\dx12`）。ツール数・サイズは、`node index.ts` を stdio で起動して `initialize` + `tools/list` を送る方法（エンジン不要・エディタ非起動）で**実測**した値。

### 1.1 三層構造と実測サイズ

```
Claude Code ──stdio(JSON-RPC/MCP)──> tools/mcp-server/index.ts ──TCP 127.0.0.1(改行区切りJSON)──> エンジン McpBridge
                                      Node 24 型ストリップ実行 / SDK 1.29.0          └> Application::HandleMcpCommand → m_mcpMethods(174 method)
```

| 項目 | 実測値 | 根拠 |
|---|---|---|
| 公開ツール数 | **220** | `tools/list` |
| `tools/list` 応答の総量 | **408,644 バイト**（CJK 文字 64,728 個） | 同上 |
| うち description / inputSchema / outputSchema | 73,115 字 / 112,796 字 / 44,899 字 | 同上 |
| description の長さ | 中央値 273 字・最大 1,837 字 | 同上 |
| 最大のツール定義 | `dx12_set_post_process` 10,496 B、`dx12_set_dxr` 8,083 B、`dx12_render_debug` 5,444 B、`dx12_quality_gate` 5,305 B | 同上 |
| outputSchema | 全ツールがほぼ同一の `{result:any}`（44,899 字がほぼ無情報） | `index.ts:106-108` |
| ツール名の最大長 | 29 文字（`mcp__dx12-engine__` を付けても 47 文字程度） | 同上 |
| 宣言している capabilities | `{"tools":{"listChanged":true}}` のみ。**instructions・prompts・resources は無い** | `initialize` 応答 |
| annotations | readOnly 91 / destructive 16 / idempotent 67（220 本中）。`git_push` `git_checkout` `git_merge` `eval_lua` `undo` などに destructiveHint 無し | `index.ts` 各 `reg()` |
| エンジン側 method | **174**（`McpDefine` 名。`"a\|b"` は 2 method に展開）。TS ツール 220 のうち **49 本は同名 method が無い合成/TS 専用ツール** | `src/core/mcp/ApplicationMcp*.cpp` |

> トークン換算は未計測。日本語主体で約 41 万バイト＝定義を全部読み込むと**コンテキストの相当部分を使う規模**。Claude Code 側は MCP ツール定義を遅延読み込み（ToolSearch）で扱う（§3）ので、実害は「毎回全量が載る」ではなく「**検索の質と往復回数**」に出る（この設計書を書いている今回のセッションでも 220 本すべてが名前だけの deferred tool として一覧されている）。

### 1.2 TS サーバ（`tools/mcp-server`）

- `index.ts` は 6,907 行・**220 ツールを 1 ファイルに直書き**。`McpServer` 生成 `index.ts:94`、登録ラッパ `regRaw` `:169` / `reg` `:194`、末尾で `StdioServerTransport` 接続 `:6906`。
- 入力は**手書き zod**。`z.object(shape).passthrough()` で受け（`:183`）、未知キーは `unknownParamKeys`（`paramGuard.ts:55`）で「近い正解つきエラー」にする（`:186-187`）。zod の既定だと未知キーが黙って捨てられ `applied:true` が嘘になる事故があったため。
- `set_*` は適用後に `get_*` で読み返して `mismatched` を返す `applyAndVerify`（`index.ts:223-247`, `paramGuard.ts:118`）。
- ブロック内で複数の `engine.call("…")` を直書きする合成ツールが 33 本、直書きが 1 つも無いツールが 29 本（VFX/ルック/デカールのライブラリ・Jev・brief のような純 TS のものと、ヘルパ関数経由でエンジンを呼ぶもの）。`COMPOSITE_TOOLS`（`paramGuard.ts:157-195`）に手で登録しないと `dx12_batch` の引数検査から漏れる。
- 純ロジックは `vfx.ts`(1,284 行)・`sceneWrite.ts`・`playtest.ts` 等に分離済み。判断段 `jev/`（外部 API `api.typesafe.ai`、`jev/client.ts:17`）は開発時専用。
- `engineClient.ts`（226 行）: 単一ソケット・遅延接続・id 相関。切断時は全 pending を reject（`:152-158`）、次の呼び出しで**ポートを再探索して再接続**（`:181`。`%TEMP%/dx12_mcp.port` → env → 8787、`:112-127`）。

### 1.3 「二重（三重）管理」の実態 — 不満 (b)(c) の根

| 同じ事実の置き場所 | 現状 | 同期方法 |
|---|---|---|
| エンジンの受理キー | `McpDefine` の第 2 引数 `"key:type,..."`（`Application.h:394-398`, `ApplicationInternal.h:950-957`）。**型のみ**。説明・必須・enum・範囲・既定値・副作用の種別は無い。`any` は「静的に決められなかった」の意味（`ApplicationMcpEditor.cpp:23-55`） | `mcp_param_spec_test`(ctest) が本文との一致を見張る |
| TS のスキーマ | 手書き zod（引数ごとに `.describe()`） | `schemaDrift.ts` が**エンジンの .cpp をテキスト解析**し（`schemaDrift.ts:1-28`）、`index.ts` も正規表現でツール定義を抜く（`schemaDrift.test.ts:102-103`）。**ドリフトを事後にテストで検出**する構造 |
| コンポーネント仕様 | `McpComponentSchema()` の手書き表（`ApplicationInternal.cpp:138`） → `describe_components`（`ApplicationMcpEntity.cpp:1167`） | 手作業 |
| Lua API | `McpLuaApi()` の**静的手書き辞書**（`ApplicationInternal.cpp:584`） → `describe_lua_api`（`ApplicationMcpEntity.cpp:1189`） | `lua-api-checklist` の 3 点セット（辞書 + `API_REFERENCE.md` + `SCRIPTING.md` + `docs/index.html`）を人が守る |
| ツール一覧 | `docs/MCP.md` §4（「全 220 ツール」`:145`）、`README.md` 表（抜粋）、`AGENTS.md` 1,993 行 | 手作業。`publish.ps1` で dx12-mcp へミラー（`CLAUDE.md:461`） |
| タイムアウト | `engineClient.ts:16-106` の method 別表（8 s の一群、15/20/30/45/60/90/120/180 s） | 手作業。新 method を足し忘れると既定 10 s |
| 「読み取り専用」判定 | エンジン `IsMcpReadOnlyMethod`（`ApplicationMcp.cpp:21-59`）と TS の `readOnlyHint` が**別々に**手書き | 手作業（不一致の検出なし） |

**（b）再起動が要る直接原因**は 2 つ: ① 新ツールを `index.ts` に足すと Node プロセスを作り直さない限り出ない（`docs/MCP.md:924`「登録後はクライアント再起動」、memory `dx12-perf-tools`「新しい MCP ツール/引数はセッションの MCP サーバを再接続するまで出ない」）。② エンジンに method を足しても TS ラッパが無ければ AI から見えない（`dx12_batch` は任意 method を撃てるが `describe_mcp_params` を引いて自分で組む必要があり、AI は選ばない）。SDK は `registerTool()` / `update()` / `remove()` のたびに `notifications/tools/list_changed` を自動送信する実装を既に持つ（`node_modules/@modelcontextprotocol/sdk/dist/esm/server/mcp.js:646,651,765`）が、**実行中に登録を変える経路がこのサーバに無い**（全ツールがトップレベルで静的に登録）。

### 1.4 プロトコルとタイムアウト

- 要求 `{"id":<正整数>,"method":<string>,"params":<object>}\n`、成功 `{"id","ok":true,"result"}`、失敗 `{"id","ok":false,"error","error_code","error_hint?","error_values?"}`（`docs/MCP.md:932-941`, `engineClient.ts:205-224`）。
- **単一クライアント**: `listen(sock, 1)`（`McpBridge.cpp:149`）。2 本目の接続はハングする（memory「セッション MCP を 1 回でも使うと接続を握る」）。127.0.0.1 限定・最初の 1 行が `{` で始まらない接続は切断（`McpBridge.cpp:62-66`）。認証なし（`MCP.md:904`）。
- **遅延応答**: ハンドラが空文字を返すと `McpBridge::Poll` は応答を送らず（`McpBridge.h:37-44`）、フレーム境界で `SendToClient` が同じ id で返す。`McpDeferred{client, requestId, idempotencyKey, method}`（`McpDeferred.h`）。対象は create/spawn/delete/duplicate/open_scene/new_scene/play/stop/undo/redo/tx_commit/rollback/screenshot 系/step_frames/imgui_pointer・key・screenshot。
- **タイムアウトは TS 側だけ**: `setTimeout` で pending を消して reject する（`engineClient.ts:208-213`）。**エンジンは処理を続行**し、**キャンセル API も進捗通知も無い**（`index.ts` の "progress" は説明文中の語のみ）。例: `imgui_pointer` 90 s / `diagnose` 180 s / `terrain_erode` 60 s。タイムアウト後に届く遅延応答は id が無いので黙って捨てられ、AI は「効いたのか」を知れない。
- **エンジン未起動/切断**: 接続失敗は 1 文（`エディタに繋がりません (host:port)`、`engineClient.ts:191`）。ポート探索・プロセス有無・`imgui.ini` 等の**自己診断は無い**。`ping` は接続できて初めて意味を持つ。
- **エラー文の口調**が方言混じり（`engineClient.ts:211` の「かもしれん」「かかるで」、`paramGuard.ts:80` の「直してくれ」）。AI 向けメッセージの語調を統一する必要がある。

### 1.5 エラーの形

- エンジン: `McpError{code, msg, hint?, validValues?}`（`ApplicationInternal.h:444-453`）→ `HandleMcpCommand` の catch（`ApplicationMcp.cpp:268-286`）。コードは 6 種のみ: 1 NOT_FOUND / 2 INVALID_PARAM / 3 MODE_CONFLICT / 4 STALE_SCENE（**未送出**、`MCP.md:800`）/ 6 UNKNOWN_COMPONENT / 7 INTERNAL（`ApplicationInternal.h:429-437`）。**未知の method も `std::exception` も INVALID_PARAM(2) に潰れる**（`ApplicationMcp.cpp:262-264,279-285`）ので、AI は「引数が悪い」のか「そのツールが無い」のか「エンジンが落ちかけ」なのかをコードで区別できない。
- TS: `err.code / hint / valid_values` に載せ替え（`engineClient.ts:215-223`）、`errResult` が**日本語のテキスト 1 本**に整形して `isError:true` で返す（`index.ts:113-121`）。`structuredContent` にエラーは載らない＝機械可読でない。
- hint/valid_values は**各ハンドラが手で付ける**（付いていないハンドラは素のメッセージのみ）。did-you-mean は「引数キー」だけ（`paramGuard.ts:34-46,68-86`）で、**値**（エンティティ名・アセットパス・enum の打ち間違い）には無い。

### 1.6 トランザクション・Undo・自動保存

- MCP 呼び出し 1 回 = Undo 1 エントリ「AI: `<method>`」。**Editor モードのみ**積む（`ApplicationMcp.cpp:133-149,137-138`）。応答の `result.undoEntry` / `undoTransaction` で分かる（`:227-238`）。
- `transaction_begin/commit/rollback/status`（`ApplicationMcpUndo.cpp:6-24`）。入れ子禁止、tx 中の `play/open_scene/new_scene/open_project` は MODE_CONFLICT（`ApplicationMcp.cpp:163-175`）、commit/rollback 応答待ち中の書き込みも拒否（`:176-186`）、600 秒放置は**確定扱い**で自動クローズ（`Application.h:1416`）。
- `dx12_batch`（`index.ts:4022-4106`）は `atomic` 既定 true で begin → 実行 → 失敗なら rollback / 成功なら commit。tx に入れない method は `TX_UNSAFE_METHODS`（`batchTx.ts:13`）で自動的に atomic を外す。
- **tx の限界**（設計上の穴）: 巻き戻しは `McpUndoTracker` が申告した ECS コンポーネントのスナップショット（`ApplicationMcp.cpp:198`, `McpUndoTrack.h`）が対象。**ファイルを書く操作は対象外**（`create_lua_component` / `create_shader` / `import_asset` / `scene_write` / デカールアトラス生成 / `blender_export`）。しかも MCP 接続中は 2 秒アイドルで**ディスクへ本保存**される（`Application.h:777`, `ApplicationMcp.cpp:291-311`）ので、メモリ上で rollback しても保存済みの状態が残りうる（前の内容は `<project>/.dx12/backups/` に 20 世代）。
- **冪等性**: `idempotency_key` は `create_entity` / `spawn_model` / `spawn_prefab` だけ（`MCP.md:774-788`, `ApplicationMcpEntity.cpp:496-505`）。`set_*` は性質上冪等だが `add_particle_layer` `scatter` `duplicate_entity` `vfx_apply`（新規）などはリトライで重複する。
- **dryRun** があるのは `organize_scene`（既定 dryRun）・`validate_layout(fix)`・`scene_write`（検証）のみ。共通機構ではない。

### 1.7 自己記述の仕組み

`describe_components`（手書き表）/ `describe_lua_api`（静的辞書）/ `describe_mcp_params`（`{key,type}` のみ）/ `describe_anim_graph` / `describe_shader_contract` / `list_shader_templates` / `vfx_library` `look_library` `decal_library`（TS 側のレシピ表）。**ツールそのものの一覧・使い分け・前提条件・次の一手を返す口は無い**。使い方の知識は description（最大 1,837 字）と `AGENTS.md`（1,993 行）に散在する。

### 1.8 テストと CI

- `npm test`（`package.json` の `scripts.test`）は 40 弱の `node *.test.ts` を直列実行。`test.ts` はモックエンジンで framing/id 相関/エラー/ポート探索/タイムアウト選択を検証するが、**「SDK のツール登録検証は省略」**（`test.ts:11-12`）。＝**ツール発見性（正しいツールが選ばれるか）・スキーマ健全性・description 長・命名規則の自動テストが無い**。
- CI: `.github/workflows/ci.yml:27-46`（`npm ci` + `npm test`、エンジン非起動）。エンジン側 ctest は `mcp_param_spec_test` / `mcp_undo_test`。
- GPU 統合: `tools/bench/golden.mjs`（固定シーン×固定カメラの決定論スクショ・基準 `tests/golden/*.png` + `baseline.json`・`--update`）、`scale_ladder.mjs`、`tools/bench/lib/engine.mjs`（ヘッドレス起動・空きポート 8850〜・PID 指定 kill）。ヘッドレス検証 `ciClient.ts`（`--launch --scenes --goals --playtests`、終了コード 0/1、`ciClient.ts:399`）。**いずれも MCP ツールとしては公開されていない**（開発者が手で回す）。

### 1.9 テストプレイ・品質・エディタ操作の現状（②③の出発点）

- プレイテスト: `record_playtest`（人のプレイ→ `.playtest`、`index.ts:2905`）/ `run_playtests`（`:2961`）。基準は「初回再生（ゴールデンラン）」。判定 3 種（終点 1.0 m / 経路 2.0 m / Lua 死亡 0）。`play_script`（台本+断言）・`autoplay`（ナビ経路の実走）・`measure_player`・`check_reachable`・`step_frames(deterministic)`。
- 品質: `validate_scene` `validate_layout` `polish_audit` `ui_audit` `perceive` `quality_gate`（`index.ts:6848`、Jev 判断段つき）。
- 性能: `perf_stats`（`ApplicationMcpEditor.cpp:781`。`cpuScopeMs`/`gpuPassMs`/`analysis`）、`benchmark`（`:875`、`uncap` 既定 true）。**閾値判定（性能ゲート）は MCP に無い**（`vg_bench.mjs` は設計書のみ、`VIRTUAL_GEOMETRY_DESIGN.md:880-917`）。
- エラー収集: `get_log`（`dx12_engine.log` を **CWD 相対**で開いて末尾 N 行を返すだけ、リング無し、`ApplicationMcpEditor.cpp:166-186`）、`get_script_errors`（live な loadError のみ）。**「前回の確認以降に増えたエラー」を取る仕組みが無く**、CWD が System32 だと `get_log` 自体が空になる。
- スクショ差分: `look_compare`（参照画像との測光比較）、`ui_compare`、`camera_path`（フレーム間差分）、`deterministic:true` の決定論撮影。**baseline の保存・命名・承認（`--update` 相当）を MCP で行う道具は無い**（`golden.mjs` は開発者用 CLI）。
- エディタ操作: MCP から触れる editor 面は `imgui_virtual_input/pointer/key/find/screenshot` の 5 本だけ（`ApplicationMcpImGui.cpp`）。`EditorCommandTable`（`kCommands` 32 件、`EditorCommandTable.h:64`）+ `window.*`（`ToolWindows.h:49` の 26 窓）+ `create.*`（`EditorCreateTable.h:27` の 23 件）＝**約 81 コマンド**があり、実行は `cmd::Execute(ctx, env, id)`（`EditorCommands.cpp:181`）に一本化されているが、MCP 経路は無い（窓を開くには `imgui_find` → `imgui_pointer` でメニューをクリックするしかない）。`UiTestHarness`（`gui/UiTestHarness.h`、ImGuiTestEngine）は診断パネルと `--ui-tests-run-all`（`main.cpp:437-443`）のみで MCP 未公開。エンジン設定 `settings.json` を MCP から読み書きする method も無い。
- 仮想ジオメトリ: `VIRTUAL_GEOMETRY_DESIGN.md` は P2 で `dx12_set/get_virtual_geometry` と `perf_stats.virtualGeometry`・`dx12_vg_dump`、P4 で `render_debug` の VG モード（`:213,762,878`）を予定。**cook（`vgeocook.exe`, `tools/vgeo-cook/`）・`.vgeo` 情報・ストリーミング状態・UE cook（`tools/ue-cook/` C#）の MCP 面は未設計**。

---

## 2. ツール分類一覧（220 本）

分類は機能で行い、本数と定義サイズ（`tools/list` 実測の JSON バイト合計ではなく、`index.ts` 内のブロック文字数。相対比較用）を付けた。**「主力」は §4.1.5 の Core 28 本に入れる候補**。

| # | カテゴリ | 本数 | 定義規模(字) | 主なツール | 所見（重複・粒度・危険・統合案） |
|---|---|---:|---:|---|---|
| A | 接続・自己記述・ログ | 11 | 9.3k | ping, get_mode, get_log, describe_components, describe_mcp_params, describe_lua_api, describe_anim_graph, describe_shader_contract, list_shader_templates, get_script_errors, diagnose | describe_* が 6 本に分散 → `dx12_describe{kind}` に統合。get_mode は ping の部分集合（重複）。**主力: ping, get_log, get_script_errors, describe_components** |
| B | エンティティ読取/検索 | 8 | 3.9k | list_entities, get_entity, find_entity, query_entities, get_hierarchy, get_bounds, list_scenes, list_assets | find_entity は list_entities(name_prefix) の部分集合。**主力: list_entities, get_entity, get_hierarchy** |
| C | エンティティ生成/編集 | 20 | 19.7k | create_entity(3.8k), spawn_box/sphere/coin, spawn_model/prefab, set_transform, set_component, remove_component, set_parent, group_entities, rename, select, focus_camera, look_at, snap_to_ground, duplicate, delete, create_prefab, scatter | **粒度不揃い**: spawn_box/sphere/coin は create_entity の糖衣。`scatter` は 4.6k 字の合成。**主力: create_entity, spawn_model, set_transform, set_component, delete_entity** |
| D | シーン/プロジェクト | 12 | 22.7k | open_scene, open_project, new_scene, save_scene, scene_write(6.9k), scene_scaffold, organize_scene, validate_naming, get/set_scene_settings, scene_env, build_game | 大量配置は scene_write が本命（`AGENTS.md` の推奨）。**危険**: new_scene/open_scene は未保存を捨てる（自動保存で緩和）、build_game は外部ビルド。**主力: open_scene, save_scene, scene_write, scene_scaffold** |
| E | マテリアル/シェーダ/デカール | 11 | 26.0k | set_pbr(2.1k), set_color, set_texture, material_apply(8.9k), set_mesh_shader(+params), set_sprite_shader, create_shader, read_shader, decal_library, decal_apply(8.5k) | material_apply が set_pbr+set_texture を畳んだ本命で、下位 3 本は AI が選び分けに迷う。**主力: material_apply, create_shader** |
| F | ライト/ルック | 5 | 7.4k | list_lights, set_sun, apply_lighting_preset, look_library, look_apply | look_apply（太陽+フォグ+空+ポスト約 20 項目）が本命。preset と重複。**主力: look_apply, set_sun** |
| G | 描画設定 get/set 対 | **26** | **32.2k** | post_process, ssao, occlusion, render_scale, depth_prepass, ssr, ssgi, contact_shadow, normal_filter, shadow_pcss, dxr, taa, volumetric_fog の各 get/set（13 対） | **最も構造が揃った重複**。全部 `{effect, settings}` の 1 対に畳める。`set_post_process` は 1 本で 10.5 KB、`set_dxr` 8 KB。**統合の効果が最大（26→2）** |
| H | VFX/演出 | 8 | 28.1k | vfx_library, vfx_apply(5.9k), vfx_preview(8.5k), list/add/remove_particle_layer, sequence_author(6.4k), sequence_preview | レシピ型(宣言的)で品質が高い。**主力: vfx_apply, sequence_author** |
| I | 地形/スカルプト | 12 | 16.4k | terrain_create/generate/sculpt/erode/paint/autopaint/set_layers/splat_info/sample, sculpt_create/make_editable/brush | 同じ道具を method 別に分けただけ → `dx12_terrain{op}` に統合可。地形は Editor 限定。**主力: terrain_generate, terrain_set_layers** |
| J | ナビ/AI/到達性 | 10 | 7.7k | navmesh_build/settings/info/path/sample/raycast/debug/clear, check_reachable, brain_state | navmesh_* は 8 本 → `dx12_navmesh{op}`。navmesh_build/clear は destructive。**主力: check_reachable** |
| K | Lua | 7 | 3.8k | create_lua_component, attach_lua_component, read_lua_component, get_lua_component_state, set_lua_property, reload_scripts, eval_lua | **危険**: eval_lua は任意 Lua 実行で destructiveHint 無し（`MCP.md:906`）。**主力: create_lua_component, attach_lua_component, get_script_errors** |
| L | ゲーム内 UI | 7 | 7.3k | ui_tree, ui_design_brief, ui_audit, ui_compose, ui_click, ui_compare, install_font | UI は compose+audit+compare のループが本命。**主力: ui_compose, ui_audit** |
| M | 再生/入力/プレイテスト | 13 | 32.0k | play, stop, step_frames, key_down/up/press, mouse_move, get_play_session, play_script(4.9k), measure_player, autoplay(8.5k), record_playtest, run_playtests | 1 手ずつ動かす key_*/step_frames と台本型が併存（AGENTS.md が後者を推奨）。**主力: play, stop, play_script, run_playtests, autoplay** |
| N | アニメ/音/物理観測 | 8 | 5.8k | play_anim, get_anim_state, set_anim_param, audio_state, get_physics_state, raycast, overlap_box, overlap_sphere | 観測系は `dx12_observe{kind}` に統合可。 |
| O | 撮影/知覚/カメラ | 17 | 34.6k | screenshot, screenshot_final, screenshot_game_view, screenshot_from, focus_and_screenshot, ui_screenshot, render_debug, camera_path, look_compare, view_texture, preview_model, pick, raycast_precise, project_world_to_screen, perceive, get/set_editor_camera | **撮影が 6 本＋αで重複**（どれが「人が見る絵」か迷う。AGENTS.md に 3 種の表がある）。**主力: screenshot_from(既定 final), perceive, look_compare** |
| P | 品質検査/性能 | 10 | 22.4k | validate_scene, validate_layout, polish_audit, quality_gate(5.3k), brief, jev_ask/eval/status, perf_stats, benchmark | **quality_gate が既に「1 回撃つ」入口**（検査のメタ層）。jev_* は開発者向け（AI の主力ではない）。**主力: quality_gate, validate_layout, perf_stats, brief** |
| Q | アセット/Blender | 11 | 16.9k | import_asset, asset_info, reload_assets, move_asset, delete_asset, blender_ensure/export/material/polish, model_brief, asset_gap | blender_* は順序が固定のワークフロー → 1 本のレシピ化の余地。delete_asset は destructive。 |
| R | エディタ UI（仮想入力） | 5 | 6.9k | imgui_virtual_input/pointer/key/find/screenshot | 低レベルで、AI が「窓を開く」だけに 3〜4 往復かかる。**editor_command で大半を置換できる（§4.6）** |
| S | Undo/Tx/batch | 7 | 7.8k | undo, redo, transaction_begin/commit/rollback/status, batch | batch(atomic) が実質の入口。tx 4 本は batch と `dx12_call` の opts に吸収可。 |
| T | Git | 9 | 4.1k | git_status/branches/checkout/merge/merge_abort/commit/push/pull/fetch | **危険**: push/checkout/merge/pull は destructive だが注釈なし。AI が使う頻度は低く、**既定では隠す（Tier 3）**。 |
| U | マルチプレイ | 3 | 1.8k | net_status, net_setup, net_launch_test_client | 用途が狭い。Tier 3。 |
| | **合計** | **220** | 317k | | |

**集計の見立て**（§4.1 の根拠）: G(26)+I(12)+J(8 のうち navmesh)+N(8) など「同じ道具の method 別」だけで約 **50 本が機械的に畳める**。C の spawn_* 3 本・A の describe_* 6 本・O の撮影 6 本・S の tx 4 本を足すと約 **75 本が構造上の重複**。定義サイズは G+E+H+M+O+P の 6 カテゴリで全体の約 55%。

---

## 3. Claude Code / MCP 仕様の事実確認（出典つき）

調査日 2026-09-30。Claude Code の docs は `code.claude.com/docs/en/*`、changelog は v2.1.284（2026-09-28）まで。**MCP spec の現行版は 2026-07-28**（`https://modelcontextprotocol.io/specification/versioning`）。原文引用は英語のまま。「公式に記載なし」は憶測で埋めず、そのまま設計上の**未確認事項（§7 のリスク・§5 M0 のプローブ）**として扱う。`list_changed` / 背景化 / タイムアウト / ツール検索 / alwaysLoad / instructions 2,048 字は、docs の生 Markdown（`https://code.claude.com/docs/en/mcp.md`）を著者が直接 grep して原文を二次確認した。その他の項目（changelog・issue・spec ページ）は調査エージェントの取得結果。

| # | 問い | 事実（版・出典） | 設計への含意 |
|---|---|---|---|
| (i) | `list_changed` で再起動なしにツールが増減するか | **docs 上は YES**。"Claude Code supports MCP `list_changed` notifications, allowing MCP servers to dynamically update their available tools, prompts, and resources without requiring you to disconnect and reconnect. … Claude Code automatically refreshes the available capabilities from that server."（`code.claude.com/docs/en/mcp` "Dynamic tool updates"）。再取得に失敗したら前回の一覧を保持（v2.1.214 以降）。導入 v2.1.0。**ただし公式リポジトリに open の食い違い報告**: [#66084](https://github.com/anthropics/claude-code/issues/66084)（2026-06、`reproduced`。list_changed 後の新ツールが ToolSearch の deferred 索引に入らず、モデルが呼べない。`/mcp` 再接続かセッション再起動で直る）、[#97369](https://github.com/anthropics/claude-code/issues/97369)（2026-09-26。一度ロードした deferred ツールは説明・スキーマが変わってもセッション中は古い定義のまま。`/mcp reconnect` でも直らず、`/compact` で直るとの報告）。いずれも報告者の主張で Anthropic の確認済み事項ではない | **list_changed を唯一の手段にしない**。ツール名を通さず呼べる安定した入口（`dx12_call` / `dx12_tool_describe`）を主経路にし、動的登録は「効けばラッキー」の最適化に留める。**中核ツールの description は固定**（#97369）し、変わる知識は `dx12_tool_describe` の返り値で配る |
| (ii) | MCP ツールが多いときのツール検索 | `code.claude.com/docs/en/mcp`: "Tool search is enabled by default: MCP tools are deferred and discovered on demand."／"Only tool names and server instructions load at session start"／"Claude Code doesn't impose a fixed per-server tool cap; the practical limit is your context window budget."。`ENABLE_TOOL_SEARCH`: 未設定=全 deferred、`auto`=定義がコンテキストの 10% 未満なら先読み・以上なら全 deferred、`auto:N`=閾値 N%、`false`=全先読み。`ANTHROPIC_BASE_URL` が第三者ホストだと既定で無効。`alwaysLoad: true`（サーバ設定）と、ツール個別の `_meta["anthropic/alwaysLoad"]: true` で常時ロード。検索方式は Claude Code docs に "matches queries against tool names and descriptions"（`agent-sdk/tool-search`）とだけあり、API 側 docs には regex / BM25 の 2 版があり検索対象は "tool names, descriptions, argument names, and argument descriptions"、既定で最大 5 件返す。どちらを Claude Code が使うかは**公式に記載なし** | dx12 の 220 本は**既にすべて deferred**（本セッションの一覧も名前のみ）。困りごとは文脈量ではなく「似た名前 26 本から検索で当てる」精度と往復。**名前・説明の語彙を検索向けに設計**（動詞+対象、日英キーワード、先頭 1 文に要点）。**サーバ `instructions` は今は空**なので、ここに「いつ何を検索するか」を書く（§4.1）。少数の shell ツールは `_meta["anthropic/alwaysLoad"]` で常時ロード |
| (ii-b) | 名前・説明の推奨 | "Tool names should be concise and descriptive. Avoid generic names like `run` or `execute`."／"Descriptions should clearly state what the tool does…"（`agent-sdk/tool-search`）。"Claude Code truncates each tool description and each server's instructions at 2,048 characters by default. Keep them concise, and put critical details near the start."（`mcp`。`CLAUDE_CODE_MAX_MCP_DESCRIPTION_LENGTH` で変更可、v2.1.280 以降）。instructions に書くべきは "What category of tasks your tools handle / When Claude should search for your tools / Key capabilities your server provides"。数値目安（SDK docs）: "50 tools can use 10-20K tokens"、"Tool selection accuracy degrades with more than 30-50 tools loaded at once" | 現状の description は最大 1,837 字＝上限手前。**先頭 200 字に要点**、全体 600 字以内をテンプレの上限にする。**主力 33 本**（常時ロード 5 + deferred 28）は上の「30〜50 本」の範囲に収める |
| (iii) | outputSchema / structuredContent / annotations | spec（2025-11-25 tools）: outputSchema があれば "Servers MUST provide structured results that conform to this schema. Clients SHOULD validate…"、structuredContent は後方互換のため "SHOULD also return the serialized JSON in a TextContent block"。2026-07-28 で `structuredContent` は任意の JSON 値に緩和、`tools/list` に `ttlMs`/`cacheScope` が必須化、**`tools/list` は決定的な順序で返すのが SHOULD**。annotations は `title / readOnlyHint / destructiveHint / idempotentHint / openWorldHint`。"all properties in ToolAnnotations are hints… Clients should never make tool use decisions based on ToolAnnotations received from untrusted servers." **Claude Code が外部サーバの readOnlyHint を権限判定に使うかは公式に記載なし**（権限は名前ベースの allow/ask/deny ルール）。structuredContent は changelog v2.0.21 に "Support MCP `structuredContent` field in tool responses" とあるが、外部サーバで content/structuredContent のどちらをモデルへ渡すかは docs に記載なし（Agent SDK の自作ツールでは `structuredContent` 設定時に text ブロックは転送されない、と `agent-sdk/custom-tools`。外部サーバへの適用は不明）。Claude Code 独自の `_meta`: `anthropic/maxResultSizeChars`（上限 500,000）、`anthropic/requiresUserInteraction: true`（毎回確認。allow ルール・`bypassPermissions` でも出る。v2.1.199 以降）、`anthropic/alwaysLoad` | **本文テキスト（JSON 文字列）を唯一の正**にし続ける。現在の outputSchema（全ツール同一の `{result:any}`、44,899 字）は情報が無いので**削除**（MUST 適合の負担も消える）。`index.ts:104-105` のコメント「Claude Code は structuredContent を読まない」は**公式では確認できない**ので削る。権限は annotations ではなく**ツール名（動詞接頭辞）と `requiresUserInteraction`** で表現する（§4.1.2, §4.3.4）。tools/list は名前順に固定 |
| (iv) | resources / prompts の使い所 | resources: `@server:protocol://path` で明示参照。対応サーバには一覧・読み取りツールも自動提供（"Claude Code automatically provides tools to list and read MCP resources when servers support them"）。prompts: `/mcp__server__prompt` のスラッシュコマンドとして出て、結果は会話に直接注入。spec も "Prompts are designed to be user-controlled"。`list_changed` は tools/prompts/resources すべて対象。v2.1.274 で「`listChanged` を宣言せずに通知するサーバの prompts/resources が更新されない」不具合が修正（tools について同種の記述は無い） | 「使い方ガイド」の**主経路は prompts/resources ではない**（prompt はユーザー起動、resource は `@` か一覧ツール経由でモデルは自律的に読まない）。主経路 = `instructions`（開始時に必ず載る・2,048 字）+ 常時ロードの `dx12_guide` ツール。prompts は「ユーザーが打つワークフロー起動コマンド」（`/mcp__dx12-engine__quality_check` 等）として補助的に配る |
| (v) | 1 サーバのツール数の実務上限 | 固定の上限は**無い**（上記）。`MAX_MCP_OUTPUT_TOKENS` 既定 25,000（警告は 10,000 超）。`MCP_TIMEOUT`（起動）既定 30 s。`MCP_TOOL_TIMEOUT` 既定 約 28 時間。ツール説明・instructions は 2,048 字で切り詰め。inputSchema のトップレベルのプロパティ名は 1〜64 字・`A-Za-z0-9_.-` のみ（違反ツールは除外、v2.1.216 以降）。ルートの `anyOf/oneOf/allOf` は平坦化される。Claude API のツール名は `^[a-zA-Z0-9_-]{1,128}$` | ツール数の上限で困ることは無い。効くのは検索精度と定義の質。**引数名は 64 字・英数字のみ**を lint で強制。ルートに `oneOf` を使わない（`op` 判別子 + フラットな properties にする） |
| (vi) | progress / cancel / 長時間ツール | **progress**: "A tool call to an MCP server that sends no response and no progress notification for the idle window aborts with an error"＝progress は**アイドルタイマーを延長**する（既定: stdio 30 分、HTTP 系 5 分、v2.1.203 以降）。ただし "The per-server `timeout` is a hard wall-clock limit per tool call, and progress notifications from the server don't extend it."。**背景化**: "An MCP tool call in the main conversation that is still running after two minutes moves to a background task instead of blocking the session."（v2.1.212 以降。`CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS`）。**サブエージェント経由の呼び出しは背景化されない**。**cancel**: spec 2026-07-28 は stdio で `notifications/cancelled` を送るのが MUST。Claude Code がユーザー中断時に送るかは**公式に記載なし**。tasks（MCP の非同期タスク拡張）への Claude Code の対応は**公式に記載なし** | 2 分超の呼び出しは、メイン会話では Claude Code が背景化してくれるが、**サブエージェント（並列に振る前提のこの計画の主役）では背景化されない**。→ 長い処理は自前の**ジョブ API（start/status/cancel）**で非同期化する（§4.3.5）。progress 通知は「アイドル打ち切り防止 + 表示」に使う（効果は限定的と見て、ジョブ API を主にする）。cancel は SDK が渡す `extra.signal` に応答する**ベストエフォート**に留める |
| (vii) | spec 2025-11-25 以降の tools 周りの追加 | 2025-11-25: `icons`、ツール名ガイダンス（1〜128 字）、入力検証エラーを Tool Execution Error で返してモデルが自己修正できるように（SEP-1303）、JSON Schema 2020-12 既定、URL 版 elicitation、tasks（実験）。2026-07-28: `initialize` と `Mcp-Session-Id` を廃止（ステートレス化）、`server/discover` 必須、変更通知は `subscriptions/listen` で opt-in、tasks を拡張へ移動、elicitation/sampling/roots を MRTR へ置換。**Claude Code**: elicitation（form/URL）に対応。MCP クライアント実装は v1（SDK 1.x）と v2（2026-07-28）の 2 系統で、**stdio サーバには既定で新リビジョンを問い合わせない**（`MCP_PROTOCOL_NEGOTIATION=auto` で問い合わせ）。tasks/sampling の対応は公式に記載なし | 現行 SDK 1.29.0（`initialize` 方式）のままで Claude Code と繋がり続ける。2026-07-28 への移行は**別課題**（§7 R7）。入力検証エラーは「モデルが読んで直せる形」で返せ、という spec の方向は §4.3 の構造化エラーと同じ。elicitation は破壊的操作の確認に使えるが、v1 runtime/stdio での可用性が未確認なので**任意機能**扱い |
| (viii) | ツール名と権限指定 | 名前は `mcp__<server>__<tool>`（本サーバは `mcp__dx12-engine__dx12_xxx`）。allow は `mcp__server`（サーバ全体）/ `mcp__server__*` / `mcp__server__tool` / **リテラルの `mcp__<server>__` の後ろにだけワイルドカード可**（`mcp__github__get_*` は有効、`mcp__*` の allow は警告つきで無視）。deny/ask は任意の glob。`mcp__` ルールに括弧（引数条件）を付けると読み込み時にスキップされる。完全名の長さ制限は Claude Code docs に**記載なし**（API は 128 字） | **読み取り専用ツールを `dx12_get_*` / `dx12_list_*` / `dx12_describe_*` … の動詞接頭辞で統一**すれば、利用者が `mcp__dx12-engine__dx12_get_*` のような 1 行で「読み取りだけ自動許可」を書ける。**`dx12_call` の 1 本化は名前ベースの権限を無効化する**（許可した瞬間に全メソッドが通る）ので read/write/guarded を分ける（§4.1.2） |

**実機で確認済みの事実（本調査の副産物）**: この設計書を書いたセッションでは `mcp__dx12-engine__*` の 220 本すべてが deferred tool（名前だけ一覧され、ToolSearch で引いて初めてスキーマを得る）として扱われていた。＝(ii) の「既定で deferred」は本環境でも成立している。

**未確認のまま残す事項**（§5 の M0 で自動プローブ化して埋める）: ① 実行中に追加したツールが `ToolSearch` に載り呼べるか（#66084 の再現有無。SDK 1.29 の stdio・Claude Code 現行版で）／② `structuredContent` と text の扱い／③ 中断時に `notifications/cancelled` が届くか／④ progress の表示と idle 延長。プローブ結果次第で §4 の「動的登録」「progress」「cancel」の採否を確定する（採否によらず主経路は `dx12_call` / ジョブ API で動くように設計してある）。

---

## 4. 設計

### 4.0 推奨案の全体像と却下案

**推奨案 = 「エンジン登録表（マニフェスト）を唯一の真実にした、3 層のツール面」**

```
                      ┌──────────────────────────────────────────────────────────┐
  Claude Code ──MCP──>│ Shell 5 本（常時ロード・定義は固定）                         │
  (stdio)             │  dx12_tool_search / dx12_tool_describe / dx12_call /        │
                      │  dx12_doctor / dx12_guide                                   │
                      ├──────────────────────────────────────────────────────────┤
                      │ Core 28 本（deferred・名前は動詞規約・説明はテンプレ）        │
                      │  頻出タスクの 9 割。宣言的な複合ツールを含む                   │
                      ├──────────────────────────────────────────────────────────┤
                      │ Long tail（~190 本相当）: MCP ツールとしては既定で出さない     │
                      │  dx12_tool_search で見つけ dx12_call で撃つ。旧 220 名は      │
                      │  エイリアスで恒久サポート（DX12_MCP_TOOLSET=full で全量登録） │
                      └───────────────▲──────────────────────────────────────────┘
                                      │ マニフェスト（名前・要約・カテゴリ・副作用・引数スキーマ・
                                      │ timeout・例・次の一手・別名）
            ┌─────────────────────────┴──────────────┐   ┌──────────────────────────────┐
            │ エンジン: McpDefine + McpMeta            │   │ TS: composites/*.ts           │
            │  describe_mcp_manifest / ping.manifestHash│  │  （複合ツール。同じ ToolSpec 型）│
            └─────────────────────────────────────────┘   └──────────────────────────────┘
```

| 案 | 判定 | 理由 |
|---|---|---|
| **A. 3 層（Shell + Core + Long tail）+ マニフェスト駆動** | **採用** | (a) 名前が競合する群（get/set 26 本・撮影 6 本・地形 12 本）を機械的に畳める。(b) 新メソッドは `dx12_call` / `dx12_tool_describe` から即使える（list_changed に依存しない）。(c) エラー・タイムアウト・副作用の型をマニフェストに一元化できる。既存の `describe_mcp_params` / `paramSpec` / `COMPOSITE_TOOLS` / `schemaDrift` の延長線上で、置き換えではなく**昇格** |
| B. 220 本のまま description だけ改善 | 却下 | Claude Code は既に deferred 化している（§3 (ii)）ので文脈量の問題ではなく、**似た名前の競合と手作業二重管理**が本質。説明を磨いても (b)(c) は残り、220 本の説明を保守する負担が増える。ただし「description テンプレ」自体は Core に適用する |
| C. `dx12_call` 1 本（数本）に一本化 | 却下 | AI が引数スキーマという最強の手がかりを失う／名前ベースの権限（§3 (viii)）が無効になり `dx12_call` を許可した瞬間に `eval_lua` `git_push` まで通る／Anthropic 推奨の「具体的で一意な説明」（§3 (ii-b)）に反する。**長尾用の抜け道としてだけ残す** |
| D. list_changed だけで動的登録 | 却下（補助としては採用） | docs は対応と書くが open issue #66084 / #97369 がある。唯一の手段にすると再起動問題が残るリスクを負う |
| E. 生成した TS を配布物に焼き込む（ビルド時コード生成のみ） | 却下（snapshot としては採用） | 生成しても新メソッドの反映に MCP サーバ再起動が要り (b) が解けない。**オフライン起動用の `manifest.snapshot.json`** としてだけ使う |
| F. MCP をエンジン内蔵の HTTP(Streamable HTTP) サーバにする | 却下（将来課題） | 認証・複数クライアント・spec 2026-07-28 対応が要り工数が大きい。今回の不満 (a)(b)(c) と無関係。単一クライアント制約（`McpBridge.cpp:149`）は R6 で別扱い |
| G. 使い方ガイドを prompts/resources に載せてツールを減らす | 部分採用 | prompt はユーザー起動、resource は `@` 参照（§3 (iv)）でモデルが自律的に読まない。主経路は `instructions` + `dx12_guide`。prompts は「ユーザーが打つ起動コマンド」として補助 |
| H. TS 手書き zod を維持して schemaDrift を強化 | 却下 | 二重管理を温存する。M3 で段階的にマニフェスト生成へ移行 |
| I. ツール名の全面リネーム | 却下 | 許可リスト（`mcp__dx12-engine__dx12_*`）・AGENTS.md・利用者の習慣を壊す。旧名は alias、新名は Core のみ |

---

### 4.1 発見性（不満 (a)）

#### 4.1.1 サーバ `instructions`（今は無い）

`initialize` 応答に `instructions` を載せる（SDK: `new McpServer(info, { instructions })`）。**2,048 字で切られる**（§3 (ii-b)）ので 1,200 字前後に収める。草案:

```
DX12 自作エンジン（エディタ）を操作する。ゲームのシーン/マテリアル/ライト/地形/UI/Lua の制作、
Play・プレイテスト・スクショ・性能・品質検査、エディタ UI の操作、仮想ジオメトリ(.vgeo)の取り込みを扱う。
■ 最初に: dx12_doctor（接続と版の確認。エンジンが落ちていれば原因と起動手順を返す）。
■ ツールの探し方: 目的が Core（dx12_* の主力 28 本）に無ければ dx12_tool_search で検索 →
   dx12_tool_describe で引数と注意点を確認 → dx12_call で実行。旧ツール名(dx12_set_ssao 等)もそのまま渡せる。
■ 作る: まとまった配置は dx12_apply_scene_spec（仕様 JSON → 1 トランザクションで生成し検証まで）。
   1 体ずつ create_entity を並べない。作業の区切りで dx12_quality_gate。
■ 直す: エラーは code / fix / didYouMean を読んでそのまま撃ち直せる。同じ呼び出しを繰り返さない。
■ エディタ UI: dx12_editor_command で足りる操作に dx12_imgui(仮想入力)を使わない。
   実マウス/実キーボード/前面化は絶対にしない（人のカーソルを奪う）。--background で起動する。
■ 危険操作(git push/eval_lua/delete_asset 等)は dx12_call の guarded 扱い。確認が要る。
詳しい手順: dx12_guide {topic: "build_scene" | "test" | "lighting" | "editor" | "vg" | "errors"}
```

> `dx12_guide` は 8〜10 トピックの Markdown を返す（現行 `AGENTS.md` の節を分割して機械的に切り出す。§6）。

#### 4.1.2 ツール名の規約（lint で強制）

`dx12_<動詞>_<対象>`（対象は複数語なら `_` 区切り、全体 32 字以内、引数名は 64 字以内の英数字）。**動詞は閉じた集合**で、動詞が副作用クラスを決める。

| 動詞（接頭辞） | 副作用クラス（`effect`） | annotations | 権限の書き方の例（利用者側） |
|---|---|---|---|
| `get_` `list_` `find_` `describe_` `check_` `validate_` `query_` | `read`（状態を変えない） | `readOnlyHint:true` | `mcp__dx12-engine__dx12_get_*` を allow |
| `capture_` | `read`+ファイル出力（撮影） | `readOnlyHint:false, idempotentHint:true` | 個別 allow |
| `set_` `create_` `add_` `remove_` `apply_` `edit_` `spawn_` `delete_` `move_` `write_` `attach_` | `write_scene`（Undo 可）／`write_file`（Undo 不可・journal 付き）／`write_setting` | `destructiveHint` は delete/remove/apply のみ true | 個別 allow |
| `play_` `stop_` `run_` `record_` `step_` | `runtime`（実行状態を変える） | `readOnlyHint:false` | 個別 allow |
| `git_` `eval_` `build_` `launch_`（プロセス起動）`publish_` | `guarded`（外部影響/任意コード/取り返しがつかない） | `destructiveHint:true, openWorldHint:true` + `_meta["anthropic/requiresUserInteraction"]:true` | 毎回確認 |
| shell の `tool_search` `tool_describe` `doctor` `guide` | `read` | `readOnlyHint:true` | allow 推奨 |
| **`dx12_call`** | **呼び先の effect に従う。`guarded` は `confirm` が無いと拒否** | `readOnlyHint:false` | 下記の分割 |

`dx12_call` は許可リストの抜け穴になり得る（§3 (viii)）。**2 本に分ける**: `dx12_call`（effect が read / write_scene / write_setting / write_file / runtime のものだけ。guarded を撃つと `E_GUARDED` を返し `dx12_call_guarded` を案内）と、`dx12_call_guarded`（`requiresUserInteraction:true`。git push・eval_lua・delete_asset・build_game 等。人が毎回承認）。read 専用の呼び出し口は作らない（read は `dx12_get_*` 群と shell 5 本で足りる。本数を増やさない）。**エンジン側のディスパッチャも `effect=guarded` の method を、要求に `guarded:true` が無ければ拒否**する（TS を迂回して生 TCP で撃たれても同じ。多層防御）。

#### 4.1.3 説明文テンプレ（Core は 600 字以内、先頭 200 字に要点）

```
<何をするかを 1 文（動詞+対象。検索に効く日英キーワードを含める）>。
使う: <場面>。使わない: <場面>（代わりに dx12_xxx）。
前提: <Editor 限定/Playing 可 など>。副作用: <読み取りのみ | シーン変更(Undo 1 回で戻る) | ファイル書込(Undo 不可) | …>。
返り値: {主要キー}。次の一手: <dx12_yyy>（<条件>）。
```

実例（`dx12_set_render_settings`。現行 `set_post_process` 10.5 KB・`set_ssao` 等 13 本の置換）:

```
描画設定（ポスト/SSAO/SSR/SSGI/TAA/フォグ/PCSS/DXR など）を 1 つ変更し、読み返した実値を返す。post process, bloom, tonemap, exposure, ambient occlusion, reflection, fog, shadow の調整に使う。
使わない: 太陽・空・霧・ポストを一括で雰囲気に寄せる → dx12_look_apply。
前提: Editor/Play どちらも可（Play 中の変更は dx12_stop で破棄され、返り値に discardedOnStop:true）。副作用: シーン設定を変更(Undo 可)。
引数: target(post_process|ssao|ssr|ssgi|taa|volumetric_fog|shadow_pcss|dxr|contact_shadow|occlusion|depth_prepass|normal_filter|render_scale) と values{…}。values の有効キーは dx12_tool_describe {name:"dx12_set_render_settings", target:"ssao"} で引く。
返り値: {applied, current, mismatched?}。次の一手: 見た目の確認 dx12_capture。
```

**target ごとの有効キー（85 個ある post_process を含む）は description に入れず `dx12_tool_describe` で引く**。これで最大 10.5 KB の定義が 700 字前後になり、目的別に検索でも当たりやすくなる。

#### 4.1.4 shell ツール 5 本（常時ロード・定義は固定）

| ツール | 入力 | 返り値 | 実装の要点 |
|---|---|---|---|
| `dx12_tool_search` | `{query:string, category?, effect?:"read"\|"write"\|"runtime"\|"guarded", tier?:"core"\|"all", limit?:number=8}` | `{hits:[{name, tier, summary, category, effect, mode, example, replaces?:[旧名], score}], total, hint}` | TS 内のインメモリ索引（BM25 風。**日本語は文字 bigram + ASCII 単語**の併用。フィールド重み: 名前×3、要約×2、keywords×2、別名(旧名)×2、引数名×1）。外部依存なし。**0 件のときは最も近い名前/カテゴリを返す**（did-you-mean）。旧ツール名で検索してもヒットする |
| `dx12_tool_describe` | `{name:string, target?:string}`（旧名・新名どちらも可。`target` は consolidated ツールの部分スキーマ用） | `{name, tier, summary, when, whenNot:[{instead}], mode, effect, timeoutMs, cancellable, dryRun:"native"\|"txn"\|"none", params:[{name,type,required,enum?,min?,max?,default?,desc}], examples:[{args, note}], next:[{tool,when}], errors:[{code,fix}], legacy:{aliases:[...]}, callTemplate:{name,args}}` | マニフェストから生成。**AI が `dx12_call` に渡すそのままの `callTemplate`** を返す。`describe_mcp_params` / `describe_components` / `describe_lua_api` / `describe_shader_contract` は `dx12_tool_describe` 下の `kind` としても引ける（当面は各ツールも残す） |
| `dx12_call` | `{name:string, args?:object, dryRun?:boolean, txn?:"auto"\|string, idempotency_key?:string, timeoutMs?:number}` | 統一エンベロープ（§4.3.1） | 旧名/新名/エンジン method 名を解決 → マニフェストで**事前検証**（型・enum・範囲・未知キー・必須）→ `effect` 判定（guarded は拒否）→ `dryRun`/`txn` 処理 → エンジン呼び出し。旧ツールの返り値形式は変えず、新形式は `meta` に付ける |
| `dx12_doctor` | `{deep?:boolean}` | `{ok, engine:{connected, port, version, manifestHash, mode, project, scene, virtualInput}, tsServer:{version, toolset, manifestSource}, issues:[{code, fix:[...]}]}` | 接続前でも動く（§4.3.6）。**最初に必ず撃つ**入口。`ping` の上位互換 |
| `dx12_guide` | `{topic?:string}` | トピック一覧、または Markdown 本文（≤ 6 KB） | `AGENTS.md` を節単位に分割した生成物（`guides/*.md`）。トピック: `build_scene` `test` `lighting` `ui` `editor` `vg` `errors` `perf` |

**常時ロード**は `_meta: {"anthropic/alwaysLoad": true}`（ツール単位、§3 (ii)）で 5 本だけ。ここが増えると毎ターンの文脈を食うので**5 本固定**を lint で守る。

#### 4.1.5 Core 28 本（deferred。頻出タスクの 9 割）

「置換」は旧ツール（互換エイリアスとして恒久サポート）。新規は M 番号のステージで作る（§5）。

| # | Core ツール | 置換する旧ツール / 新規 | 目標 |
|---|---|---|---|
| 1 | `dx12_list_entities` | list_entities + find_entity + query_entities + get_hierarchy + get_bounds（`view:"tree"\|"flat"`, `tag`, `box`, `bounds:true`） | ① |
| 2 | `dx12_get_entity` | get_entity | ① |
| 3 | `dx12_get_render_settings` | get_post_process / ssao / ssr / ssgi / taa / volumetric_fog / shadow_pcss / dxr / contact_shadow / occlusion / depth_prepass / normal_filter / render_scale / scene_settings（**14 本 → 1**。`target` 省略で有効な全エフェクトの要約） | ① |
| 4 | `dx12_set_render_settings` | 上記の set_* 14 本（**14 本 → 1**） | ① |
| 5 | `dx12_get_log` | get_log + get_script_errors（`since` カーソル・レベル・grep） **新機能: リング化（§4.5.1）** | ② |
| 6 | `dx12_get_perf` | perf_stats + benchmark（`mode:"snapshot"\|"benchmark"`, `gate?`） | ② |
| 7 | `dx12_capture` | screenshot / screenshot_final / screenshot_game_view / screenshot_from / focus_and_screenshot / ui_screenshot / render_debug / view_texture（`view:"final"\|"scene"\|"game"\|"ui"\|"debug"`, `from?`, `entity?`, `mode?`, `deterministic?`, `path`）**8 本 → 1** | ①② |
| 8 | `dx12_apply_scene_spec` | **新規（§4.4）**: 仕様 JSON → 差分計画 → 1 トランザクションで生成 → 検証。scene_scaffold / scene_write / organize_scene / scatter を内包 | ① |
| 9 | `dx12_create_entity` | create_entity + spawn_box / sphere / coin | ① |
| 10 | `dx12_spawn_model` | spawn_model + spawn_prefab（`kind` は拡張子から自動判定） | ① |
| 11 | `dx12_set_transform` | set_transform + look_at + snap_to_ground（`snap:true`, `lookAt?`） | ① |
| 12 | `dx12_set_component` | set_component + remove_component（`remove:true`）+ set_pbr / set_color / set_lua_property（`component:"material"` 等のショートカットは Long tail に残す） | ① |
| 13 | `dx12_delete_entity` | delete_entity | ① |
| 14 | `dx12_look_apply` | look_apply（+ look_library を `dx12_tool_describe` に統合） | ① |
| 15 | `dx12_material_apply` | material_apply（+ set_texture / set_pbr の呼び分けを説明で吸収） | ① |
| 16 | `dx12_vfx_apply` | vfx_apply（+ vfx_library / preview を `dx12_tool_describe` `dx12_capture` に統合） | ① |
| 17 | `dx12_edit_terrain` | terrain_create / generate / sculpt / erode / paint / autopaint / set_layers + sculpt_create / make_editable / brush（`op`）**10 本 → 1** | ① |
| 18 | `dx12_create_lua_component` | create_lua_component + attach_lua_component（`attachTo?`）。書く前の構文検査つきなのは現行どおり | ① |
| 19 | `dx12_ui_compose` | ui_compose（+ ui_audit を `audit:true` で連結） | ① |
| 20 | `dx12_play` | play | ② |
| 21 | `dx12_stop` | stop | ② |
| 22 | `dx12_play_script` | play_script（+ step_frames / key_* の台本化を促す説明） | ② |
| 23 | `dx12_run_playtests` | run_playtests（+ record_playtest は `dx12_playtest{op}` へ拡張 §4.5.2） | ② |
| 24 | `dx12_quality_gate` | quality_gate（**新チェック追加**: errors / visual / perf / reachable §4.5） | ①② |
| 25 | `dx12_editor_command` | **新規（§4.6）**: `{op:"list"\|"run"\|"state", id?, args?}` | ③ |
| 26 | `dx12_imgui` | imgui_virtual_input / pointer / key / find / screenshot（`op`）**5 本 → 1**。呼び出しごとに OS カーソル不変を検証 | ③ |
| 27 | `dx12_open_scene` | open_scene（+ new_scene / open_project は Long tail） | ① |
| 28 | `dx12_save_scene` | save_scene（返り値に `layout` 要約が載る現行仕様を維持） | ① |

**Long tail に残す**（例）: ナビメッシュ 8 本→`dx12_navmesh{op}`（Long tail 内で統合）/ アニメ 3 / 音・物理観測 / undo・redo・transaction_*（`dx12_call` の `txn` と `dx12_batch` に吸収。`dx12_batch` は存続）/ Blender 7 / Git 9（guarded）/ マルチプレイ 3 / jev_* / decal_* / sequence_* / アセット操作 / `dx12_perceive` `dx12_validate_layout` `dx12_polish_audit`（quality_gate が内包するので通常は撃たない）。

**本数の見込み**: MCP に見えるツール = shell 5 + Core 28 + `dx12_call_guarded` 1 + `dx12_batch` 1 ≒ **35 本**（220 → 35）。`DX12_MCP_TOOLSET=full` を指定すると旧 220 名も従来どおり登録される。

#### 4.1.6 マニフェスト駆動の統合ルール（手書きの統合をしない）

マニフェストの `group` と `op` から consolidated ツールを**生成**する:

| group | 生成される Core ツール | 対象 method（例） |
|---|---|---|
| `render_setting`（op=get/set, `target` = 13+1） | `dx12_get_render_settings` / `dx12_set_render_settings` | get_ssao/set_ssao … |
| `terrain`（op = create/generate/…） | `dx12_edit_terrain`（write）/ `dx12_query_terrain`（read: sample/splat_info） | terrain_* / sculpt_* |
| `capture`（source 別） | `dx12_capture` | screenshot*, render_debug … |
| `navmesh` `observe` `net` | Long tail 側の `dx12_navmesh` `dx12_observe` … | navmesh_*, audio_state … |

**新しいエンジン method を足すと、`group`/`op` を付けるだけで consolidated ツールの `target` 列挙に自動で入る**（例: VG の `set_virtual_geometry` は `render_setting` group の `target:"virtual_geometry"` として現れる。TS の改修は不要）。

#### 4.1.7 発見性の合否テスト（機械判定）

- **オフライン（決定論・CI 常時）**: `eval/discovery_tasks.json`（代表 30 タスク×日本語言い換え 2 通り = 60 クエリ。付録 A）。`dx12_tool_search(query)` の上位 3 件に期待ツールが入る率 **recall@3 ≥ 90%**、`dx12_tool_describe` の `callTemplate` をそのまま `dx12_call` に渡して**事前検証を通る率 100%**。
- **ライブ（定期・手動）**: `claude -p`（非対話。**背景化されない**点に注意、§3 (vi)）または Agent SDK で dx12 サーバだけを繋ぎ、30 タスクを各 3 回。**最初に呼んだ dx12 ツールが許容集合に入る率 ≥ 85%** かつ M0 で測った現行 220 本の基準より**低下しない**（低下 5pt 超なら §7 R2 の撤退）。API 費用が掛かるので M0/M3/リリース前の 3 回だけ回す。
- **静的 lint**（CI 常時、`tool-surface.test.ts`）: 名前規約・動詞と annotations の整合・Core の description ≤ 600 字（全ツール ≤ 2,048 字）・引数名 ≤ 64 字英数字・ルートに `oneOf` なし・shell の alwaysLoad が 5 本・`outputSchema` なし・旧 220 名がすべて解決できる。

---

### 4.2 再起動不要（不満 (b)）

#### 4.2.1 何が「再起動」を強いていたか、どう外すか

| 変更の種類 | 現状 | 新設計 |
|---|---|---|
| エンジンに method を足す（C++） | TS ラッパを書いて Claude Code を再起動 | エンジン再ビルド・再起動だけ。TS サーバは**生きたまま**次の呼び出しで再接続し、`ping.manifestHash` の変化を検知 → `describe_mcp_manifest` を再取得 → `dx12_tool_search/describe/call` から**即使える**。Core に載せたいものだけ `expose:"core"` でマニフェストに書けば動的登録（list_changed）も試みる |
| 合成ツール（TS）を足す | `index.ts` 追記 → MCP サーバ再起動 | `tools/mcp-server/composites/*.ts` に 1 ファイル置くだけ。`DX12_MCP_DEV=1` のとき `fs.watch` + `import(url + "?v=" + mtime)` で**ホットリロード**し `registerTool/update/remove`（SDK が list_changed を自動送信、`mcp.js:646,651`）。ホットリロードは開発時のみ（既定 OFF、R13） |
| shell 5 本の変更 | 再起動 | 再起動（**まれ**にしか変えない。定義を固定する理由） |
| description の更新 | #97369 により同一セッション内は反映されない | Core の description は固定。変わる知識は `dx12_tool_describe` の返り値へ |

#### 4.2.2 マニフェスト（エンジン側 `McpDefine` の昇格）

現状 `McpDefine(names, paramSpec, fn)` の `paramSpec` は `"key:type,…"` の文字列のみ（`Application.h:394-398`）。これを **`McpMeta` 構造体を受ける多重定義**にする。**meta が無い method は paramSpec から自動導出**（型のみ・`category:"uncategorized"`）して動き続ける＝移行は段階的で、旧ハンドラは 1 行も書き換えない。

```cpp
// 例: ApplicationMcpRender.cpp（イメージ）
McpDefine("get_ssao|set_ssao", McpMeta{
    .summary   = "SSAO（画面空間の環境遮蔽）の設定を読む/変える",
    .keywords  = "ssao ambient occlusion 影 AO 遮蔽 暗さ",
    .category  = "render",  .group = "render_setting", .target = "ssao",  // op は名前の get_/set_ から
    .effect    = McpEffect::WriteSetting,  .mode = McpMode::Any,
    .timeoutMs = 8000,  .idempotent = true,
    .params    = SsaoParams(),            // X マクロ DX12E_SSAO_FIELDS から生成（現状の仕組みを流用）
    .next      = {{"dx12_capture", "見た目の確認"}},
}, DX12E_MCP_HANDLER { ... });
```

`McpParam { name, type(bool|int|number|string|vec2|vec3|vec4|entityRef|assetPath|enum|object|array), required, enum[], min, max, default, desc, unit }`。ポスト/SSAO は既に X マクロが唯一の名前表なので（`ApplicationInternal.h:970-1010`）**型・既定値・範囲をマクロ側に足せば 85 項目の説明が自動で揃う**。

新エンジン method `describe_mcp_manifest {method?, category?, since?}` → `{protocol:1, manifestHash, methods:[{name, meta…}], categories:[…]}`。`manifestHash` は `ping` にも載せる（軽量な変更検知）。`describe_mcp_params` は後方互換で残す（マニフェストの `params` から生成）。

**TS 側の複合ツール**（vfx_apply・scene_write・quality_gate など 33+29 本）は同じ `ToolSpec` 型で宣言する:

```ts
// composites/vfxApply.ts
export default defineTool({
  name: "dx12_vfx_apply", tier: "core", category: "vfx", effect: "write_scene", mode: "editor",
  summary: "…", keywords: "…", params: {...}, next: [...],
  handler: async (ctx, args) => { /* ctx.engine.call(...) / ctx.tx(...) */ },
});
```

`ToolSpec` から zod（`manifestToZod`。既存 `passthrough` 検査 `index.ts:183-188` を流用）と JSON Schema の両方を生成し、`describe` にも同じ情報が出る。**手書き zod と `COMPOSITE_TOOLS` 手登録は段階的に廃止**（M3 で「マニフェスト生成」に置換。移行前の旧ツールは従来どおり動く）。

#### 4.2.3 起動時の流れ（エンジン未起動でも tools/list が空にならない）

```
起動 → manifest.snapshot.json（リポジトリに同梱。CI で再生成）を読む → tools/list を返す（決定的順序）
     → 初回のエンジン接続/再接続で ping.manifestHash を比較
        一致 → 何もしない
        不一致 → describe_mcp_manifest を取得 → 索引と Core 登録を更新 → 変化があれば list_changed（補助）
     → エンジンが古い（TS より新しい method を要求）→ E_ENGINE_TOO_OLD（fix: エンジンを更新）
```

配布リポジトリ（`dx12-mcp`）はエンジン本体と別に配られる（`CLAUDE.md:460`）ため、**版ずれ**は常に起きる。マニフェストの `protocol` と `manifestHash` で明示的に扱うのが「壊れにくさ」に直結する。

#### 4.2.4 二重管理の解消（手作業の廃止対象）

| 現状の手作業 | 置き換え |
|---|---|
| `index.ts` の zod スキーマと `describe()` | マニフェストから生成（M3） |
| `schemaDrift.ts` によるエンジン .cpp のテキスト解析 | エンジン ctest（`mcp_param_spec_test` の拡張）が「meta.params ⇔ ハンドラ本文のキー」を検査（**エンジン側で完結**）。TS 側は「マニフェスト snapshot ⇔ 旧 zod（移行中のみ）」だけを見る |
| `engineClient.ts` の method 別タイムアウト表 | マニフェストの `timeoutMs` |
| `IsMcpReadOnlyMethod`（エンジン）と `readOnlyHint`（TS）の別管理 | `effect` 1 つから両方導出（`IsMcpReadOnlyMethod` は `meta.effect==Read` から生成） |
| `docs/MCP.md` §4 / README の表 / `AGENTS.md` のツール索引 | マニフェストから生成（`npm run gen:docs`、CI で `--check`）。手書きは「使い方・罠」だけを残す（§6） |
| `describe_components` の手書き表 / `describe_lua_api` の静的辞書 | 手書きだが `dx12_tool_describe{kind}` から一様に引ける。Lua は `lua_api.json` 単一ソース化を M4 で検討（§6） |

---

### 4.3 エラーの強化（不満 (c)）

#### 4.3.1 統一エンベロープ（新形式。旧ツールの出力は互換のため変えない）

成功: `{"ok":true,"result":…,"meta":{"tool","tookMs","undoEntry?","sceneGeneration?","warnings":[…],"next":[{tool,args,when}],"lateResults?":[…]}}`

失敗（`isError:true`、本文は JSON 文字列。AI が**読んでそのまま撃ち直せる**）:

```json
{"ok":false,
 "error":{
  "code":"E_BAD_ENUM",
  "message":"dx12_edit_terrain: op に 'dig' は使えない",
  "cause":"op は列挙値。渡された 'dig' が候補に無い",
  "didYouMean":["sculpt"],
  "validValues":["create","generate","sculpt","erode","paint","autopaint","set_layers"],
  "fix":[{"tool":"dx12_edit_terrain","args":{"op":"sculpt","brush":"lower"},"why":"'dig' に最も近い操作"}],
  "retryable":false,
  "engineCode":2,
  "docs":"dx12_tool_describe {name:'dx12_edit_terrain'}"}}
```

- `fix` は**そのまま `dx12_call` に渡せる**形（`tool`/`args`）。エンジン側が返す `error_hint`（文）は `cause`/`message` に、`error_values` は `validValues` に、それぞれ載せ替え（互換）。
- **語調は標準語・敬体なし・簡潔**に統一（現行の「かもしれん」「直してくれ」は撤去、`engineClient.ts:211` `paramGuard.ts:80`）。

#### 4.3.2 コード体系（数値は旧互換、文字列を追加）

| 文字列コード | 旧 | 意味 | `retryable` | 既定の `fix` |
|---|---:|---|---|---|
| `E_ENGINE_UNREACHABLE` | — | エンジンに繋がらない（ポート無/プロセス無/別クライアントが占有） | ○（起動後） | `dx12_doctor` の診断（§4.3.6） |
| `E_ENGINE_BUSY` | — | 別クライアントが単一ブリッジを保持（接続は通るが応答無し） | ○ | 他セッションを閉じる |
| `E_ENGINE_TIMEOUT` | — | 応答が期限内に来ない。**エンジンは処理を続けている可能性**（`lateResult` 追跡、§4.3.4） | ○（同じ `idempotency_key` で） | 同じ key で再送 / `dx12_get_log` |
| `E_ENGINE_TOO_OLD` | — | TS が要求する method/protocol をエンジンが持たない | × | エンジンを更新 |
| `E_UNKNOWN_TOOL` | 2 | ツール/メソッド名が無い（現状は 2 に潰れる、`ApplicationMcp.cpp:262-264`） | × | `didYouMean`（`dx12_tool_search` の上位） |
| `E_UNKNOWN_PARAM` `E_MISSING_PARAM` `E_BAD_TYPE` `E_BAD_ENUM` `E_OUT_OF_RANGE` | 2 | 引数不正（事前検証で TS が返す。エンジンも中央検証で返す） | × | `didYouMean` / `validValues` / 範囲 |
| `E_NOT_FOUND_ENTITY` `E_NOT_FOUND_ASSET` `E_NOT_FOUND_SCENE` `E_NOT_FOUND_COMPONENT` | 1 / 6 | 対象が無い。**近い名前を最大 5 件**添える（シーン内の名前・assets 内パスの編集距離） | × | `didYouMean` + `dx12_list_entities` |
| `E_STALE_SCENE` | 4（未送出） | `expectGeneration` を渡した呼び出しで sceneGeneration が違う（**実際に送る**。現状は未送出、`MCP.md:800`） | ○（引き直し後） | `dx12_list_entities` |
| `E_MODE_CONFLICT` | 3 | Editor/Playing が要件と合わない、tx 中に禁止 method、commit 処理中 | ○ | `dx12_stop` 等 |
| `E_MODAL_OPEN` | — | ImGui のモーダルが開いていて UI 操作できない | ○ | `dx12_editor_command{op:"state"}` |
| `E_VIRTUAL_INPUT_OFF` | 3 | 仮想入力モードが OFF（実入力と混ざるので拒否） | ○ | `dx12_imgui{op:"mode",enable:true}` |
| `E_UNSUPPORTED` | — | GPU/環境が非対応（DXR 等。**再送しても無駄**を明示） | × | 代替の案内 |
| `E_GUARDED` | — | guarded なメソッドを `confirm` 無しで要求 | × | `dx12_call_guarded` |
| `E_VALIDATION_FAILED` | 2 | 宣言的入力（scene spec 等）の検証失敗。`issues:[{path(JSON Pointer), code, message, fix}]` | × | `specPatch`（§4.4.3） |
| `E_FILE_IO` | 7 | ファイル書込/読込失敗（WIC の「path が書けない」など。**path 未指定の罠**を検出） | ○ | `path` を明示 |
| `E_CANCELLED` | — | 呼び出しが中断された | ○ | — |
| `E_SAFETY_VIOLATION` | — | 仮想入力中に OS のカーソル/前面窓が動いた（本来ありえない。§4.6.2）。以降の UI 操作を止める | × | 人に報告 |
| `E_INTERNAL` | 7 | エンジン内部エラー。直前の `dx12_get_log` の error 行を `details` に付ける | 場合による | `dx12_get_log` |

エンジン側は `McpErr` に `UnknownMethod=8, Busy=9, Unsupported=10, Guarded=11, Cancelled=12, ModalOpen=13, FileIo=14` を追加し、`McpError` に `fix`（構造化）/ `didYouMean` / `details` を足す。応答は `error_fix` / `error_did_you_mean` / `error_details` として**加算**（旧クライアントは無視）。

#### 4.3.3 パラメータの事前検証

1. **TS（`dx12_call` / Core ツール共通）**: マニフェストで型・enum・範囲・必須・未知キー（既存 `unknownKeyError` の did-you-mean を流用、`paramGuard.ts:68`）を検査。ここで弾けるものは往復ゼロで返る。
2. **エンジン（ディスパッチャ 1 箇所、`ApplicationMcp.cpp:189-200` の直前）**: meta があれば required/enum/range/type を**中央で**検査してからハンドラへ。生 TCP・`dx12_batch` の抜け道も塞ぐ。各ハンドラが手で `McpError` を組む現状を減らす。
3. **値の did-you-mean**: `ResolveMcpEntity`（`ApplicationInternal.h:528-533`）、アセットパス、シーン名、コンポーネント jsonKey、Lua の `entity` 名で、見つからないときに**近い候補**（編集距離。シーン内名前は 1 万体でも O(N) で十分）を `didYouMean` に入れる。

#### 4.3.4 副作用の安全性（冪等・dryRun・自動ロールバック）

**副作用クラス（`effect`）を全 method に付ける**（§4.1.2）。クラスごとの保証:

| effect | 例 | Undo | ロールバック | 冪等 | dryRun |
|---|---|---|---|---|---|
| `read` | get/list/describe | 不要 | 不要 | 自明 | 不要 |
| `write_scene` | set_transform / create_entity / set_component | ○（MCP 1 回=1 エントリ、`ApplicationMcp.cpp:133-149`） | tx で丸ごと（現行） | **全 write に拡張**（下） | `txn` プレビュー（下） |
| `write_setting` | set_ssao / set_sun / set_scene_settings | ○（設定フィンガープリント経由） | tx（現行の範囲） | 自然に冪等 | `txn` プレビュー |
| `write_file` | create_lua_component / create_shader / import_asset / scene_write / decal atlas | **×** | **file journal（新設）** | key で再送を無害化 | 検証のみ（`dryRun:"native"` は scene_write 等が既に持つ） |
| `runtime` | play / stop / step_frames | Play/Stop がスナップショットに戻す | — | — | — |
| `guarded` | git_push / eval_lua / delete_asset / build_game | × | × | × | × |

- **`idempotency_key` を全 write に一般化**: ディスパッチャで `(method, key)`→結果をシーン世代内でキャッシュ（現行は create/spawn だけ、`ApplicationMcpEntity.cpp:496-505`。上限 256 件・シーン切替で破棄は現行踏襲）。**`dx12_call` は write 系で自動採番**し、`E_ENGINE_TIMEOUT` のときに**同じ key で自動再送**して「実は完了していた」を `idempotentReplay:true` で回収する（二重生成を作らない）。
- **`dryRun`（3 種）**: `native`（method 自身が持つ: organize_scene の dryRun / validate_layout / scene_write の検証。マニフェストで宣言）、`txn`（**begin → 実行 → 結果と変更エンティティの差分を取得 → rollback**。`write_scene`/`write_setting` かつ Editor モードかつファイルを書かない method に限る。`McpUndoTracker` の変更スナップショットから `changed:[{entity,components}]` を作る。**tx 中は自動保存を保留**する変更をエンジンに入れる＝2 秒アイドル本保存 `Application.h:777` がプレビュー中の状態を書かないため）、`none`（不可なら `dx12_tool_describe` に明記して `dryRun:true` は `E_UNSUPPORTED`）。
- **自動ロールバック**: `dx12_call{txn:"auto"}` と scene spec の適用、`dx12_batch`（現行 atomic）が共通の `withTransaction(label, fn)` を使う。失敗→rollback、成功→commit、戻り値に `transaction:{committed|rolledBack, calls, undoEntry}`。
- **file journal**: `write_file` の method は `McpWriteFile(path, bytes)` を通し、tx 中は「直前の内容（または不在）」を `<project>/.dx12/journal/<txId>/` に退避する。rollback で復元、commit で破棄。**tx の対象外だった穴**（§1.6）を塞ぐ。未対応の method は `effect:"write_file"` に `journal:false` と明記し、`dx12_batch atomic` の**対象から自動で外す**（現行 `TX_UNSAFE_METHODS` の一般化）。

#### 4.3.5 遅延応答のタイムアウト・進捗・キャンセル・ジョブ

現状（§1.4）: TS が黙って諦め、エンジンは続行、遅れて届いた応答は捨てられる。

1. **タイムアウトはマニフェスト由来**（`timeoutMs`）。`dx12_call{timeoutMs}` で上書き可。
2. **遅延応答の回収**: タイムアウト後に届いた応答（id 不一致）を捨てず**遅延結果ジャーナル**（直近 50 件・メモリのみ）に入れる。`E_ENGINE_TIMEOUT` のエラーに `{method, elapsedMs, engineStillBusy}`（短い `ping` で判定）を付け、次の呼び出しの `meta.lateResults` に「`open_scene` はタイムアウト後に完了した: result…」を載せる。
3. **進捗通知**: エンジンは長い method（step_frames / render_debug / diagnose / terrain_erode / imgui drag / benchmark / perceive 等）で、同じ id の**中間行** `{"id":N,"progress":{"done":a,"total":b,"note":"…"}}` を送れる（`McpDeferred` に `Progress()` を足す。`McpBridge::SendToClient` は既に任意の行を送れる）。TS の `engineClient.onData` が `progress` を見て、リクエストに `progressToken` があれば `notifications/progress` を送る。**Claude Code の idle タイマー延長（stdio 30 分）と表示に使う**（§3 (vi)）。効果は限定的と見て、主役は次のジョブ API。
4. **キャンセル**: SDK が渡す `extra.signal`（AbortSignal）が abort されたら TS が `{"method":"cancel","params":{"target":id}}` を送り、エンジンは cancellable な method のフラグを落とす（step_frames の残りフレームを 0 に、deterministic 撮影の待ちを打ち切る等）。**Claude Code が `notifications/cancelled` を送るかは未確認**（§3）なので**ベストエフォート**。効かなくてもタイムアウトが最終防衛線。
5. **ジョブ API（長時間処理の主役）**: `dx12_job {op:"start"\|"status"\|"cancel"\|"list", kind?, args?, jobId?}`。`start` は即座に `jobId` を返す。TS サーバがジョブ表（メモリ + `<project>/.dx12/jobs/<id>.json`）を持ち、`status` は `{state, progress, log(tail), result?, error?}`。対象: `vg_cook`、`ci_run`（全検査）、`benchmark` 長尺、`blender_export`、`ue_import`、UI 自動テスト全件。**理由**: Claude Code の自動背景化（2 分）は**メイン会話だけ**で、サブエージェント・非対話（`claude -p`）では効かない（§3 (vi)）。本計画は複数エージェントへの並列委譲が前提なので、明示的な非同期 API が要る。外部プロセスの kill は**自分が起動した PID だけ**（`taskkill /PID <pid> /T /F`。既存 `ciClient.ts:409-423` の作法）。

#### 4.3.6 エンジン未起動/切断時の自己診断と再接続

`dx12_doctor` と、`E_ENGINE_UNREACHABLE` のエラーが返す `details` は同じ診断:

```json
{"ok":false,"error":{"code":"E_ENGINE_UNREACHABLE","cause":"ポート 8787 に応答なし",
 "details":{
  "portFile":{"path":"%TEMP%\\dx12_mcp.port","value":8790,"ageSec":5120},
  "candidates":[{"port":8790,"connect":"refused"},{"port":8787,"connect":"refused"}],
  "process":{"running":false,"note":"DX12Engine.exe が見つからない"},
  "lastLog":"…dx12_engine.log の末尾 5 行（あれば）",
  "versions":{"ts":"0.8.0","protocol":1,"engineManifestHash":null}},
 "fix":[{"tool":"dx12_launch_engine","args":{"project":"<最後に使ったプロジェクト>","mode":"background"},"why":"--background で起動（前面に出ない）"}]}}
```

- 診断項目: ①ポートファイル（存在・更新時刻）②候補ポート（env / ファイル / 8787-8797 / ヘッドレス用 8850-8899）への **connect のみ**（`ping` は送らない。**単一クライアント**なので、別セッションが握っている場合 connect は通っても応答が無い。1.5 秒応答無しなら `E_ENGINE_BUSY` と判定）③`DX12Engine.exe` のプロセス有無と起動引数（`--background/--headless/--project`）④`dx12_engine.log` の末尾 ⑤版（TS の manifest snapshot ⇔ エンジンの `manifestHash`）⑥（deep）仮想入力モードの状態・保留中のモーダル。
- `dx12_launch_engine`（guarded 相当の確認つき・Long tail）: `DX12Engine.exe --background --project <dir> --mcp-port <N>` を起動して**ポートが開くまで待つ**（`ciClient.launch` / `bench/lib/engine.mjs` の作法: CWD は exe のフォルダ、PID 指定 kill）。**前面化・最大化はしない**（`AGENTS.md` の最重要ルール）。
- **再接続**: 現行の遅延接続 + ポート再探索（`engineClient.ts:181`）を維持し、接続失敗時に 0.3/0.6/1.2 s の短い再試行を入れる（エンジン再起動直後の窓）。再接続で `manifestHash`/`engineStartedAt` が変わっていたら**キャッシュ（冪等キー・エンティティ id のヒント）を破棄**し、次の結果の `meta.warnings` に「エンジンが再起動した。entityId は失効している」を載せる。

---

### 4.4 AI にゲームを作らせる（目的 ①）

#### 4.4.1 宣言的な複合ツール `dx12_apply_scene_spec`

AI が 1 体ずつ `create_entity` を並べる（1 体 1 フレーム。`AGENTS.md:723-727`）代わりに、**シーン仕様 JSON を 1 回で渡す**。仕様の主キーは安定した `id`（名前規約 `<PREFIX>_<Kind>_<NN>` は自動採番）:

```json
{
 "version": 1,
 "brief": {"intent": "廃墟のショッピングモール。ホラー。暗く湿った空気", "budget": {"fps": 60, "maxDraws": 3000}},
 "look":  {"preset": "horror_dusk", "strength": 0.8},
 "entities": [
   {"id": "floor", "group": "LVL", "kind": "box", "scale": [24, 0.3, 40], "material": {"keyword": "concrete"}},
   {"id": "pillar", "group": "LVL", "kind": "model", "model": "models/pillar.gltf",
    "place": {"pattern": "grid", "on": "floor", "count": [4, 6], "spacing": [6, 6], "jitter": 0.1}},
   {"id": "torch", "group": "FX", "fx": {"recipe": "torch", "scale": 1.0}, "place": {"near": "pillar", "offset": [0.6, 1.8, 0]}},
   {"id": "player", "group": "GP", "template": "fps_player", "at": [0, 1, -16]},
   {"id": "exit", "group": "GP", "kind": "trigger", "at": [0, 1, 18], "role": "goal"}
 ],
 "decals": [{"id": "stains", "recipe": "grime", "on": "floor", "count": 12}],
 "ui": {"blueprint": "hud_minimal"},
 "checks": {"layout": "safe-fix", "reachable": {"from": "player", "to": "exit"},
            "screenshots": [{"label": "入口", "camera": {"position": [0, 1.7, -16], "target": [0, 1.7, 0]}}],
            "errors": true, "perf": "brief"}
}
```

引数: `{spec | recipe+params, mode:"validate"\|"plan"\|"apply", verify?:true, txn?:true(既定), patch?:JSON-Patch}`。

#### 4.4.2 処理の流れ（1 トランザクション）

```
validate ──> plan ──> apply(tx) ──> verify ──> report
 (1)            (2)         (3)            (4)         (5)
```

1. **validate**（副作用なし）: JSON Schema + 意味検査（`id` 重複・参照先の存在・アセットが `list_assets` に実在・名前規約・グループ・単位（cm/m の取り違え疑い））。エラーは `issues:[{path:"/entities/1/model", code:"E_NOT_FOUND_ASSET", didYouMean:["models/pillar_01.gltf"], fix:{…}}]` の形（JSON Pointer 付き）。**現行 `scene_write` の検証（`AGENTS.md:744-751`）を土台にして一般化**。
2. **plan**: 現在のシーン（`id`→名前→エンティティの対応は各エンティティの `DataComponent` に `specId` を書いて保持）との**差分**を `{create:[], update:[], delete:[], unchanged:N}` で返す。**同じ仕様を撃ち直しても no-op**（冪等）、仕様を 1 行直せば最小差分だけ動く。
3. **apply**: `withTransaction`（§4.3.4）で実行。規模で経路を選ぶ: 少数はライブ操作（`create_entity` 等の遅延同期）、**40 体超は `scene_write` 経路**（シーン JSON を組み立てて 1 回で `open_scene`。自動保存との競合を避けるため**書いたら即 open、事前に `sceneDirty:false` を確認**、`AGENTS.md`/memory の罠を実装内に取り込む）。失敗 → rollback（＋file journal）。
4. **verify**: `validate_layout(fix:"safe")`・`validate_naming`・到達性（`check_reachable`）・エラー収集（§4.5.1）・任意で `dx12_capture` の決定論撮影。
5. **report**: `quality_gate` と同じ形（`pass, blocking[], keep[], uncertain[], suggestions[]`）+ `plan`/`applied` の要約 + `next`。

#### 4.4.3 失敗時に AI が自己修正できるフィードバック

- 各 issue に **`specPatch`**（RFC 6902 JSON Patch。例: `[{"op":"replace","path":"/entities/1/model","value":"models/pillar_01.gltf"}]`）を付け、AI は `dx12_apply_scene_spec {spec, patch:[…]}` で**そのまま撃ち直せる**。
- `blocking` が残ったら適用は**ロールバック済み**（部分適用を残さない）で、`report.reasons` に「どの `id` がどの規則で落ちたか」を 1 行ずつ返す。
- `uncertain`（目で見て判断する項目）は `look:{tool:"dx12_capture",args:{…}}` を添える（`quality_gate` の作法）。

#### 4.4.4 既存ツールとの関係（作り直さず内包する）

| 既存 | 位置づけ |
|---|---|
| `scene_scaffold`（グループ根 7 個） | apply が暗黙に呼ぶ（冪等） |
| `scene_write`（大量配置 JSON・検証・バックアップ） | apply の一括経路の実装。単体ツールは Long tail に残す |
| `organize_scene` / `validate_naming` | `spec.organize:true` と verify に内包 |
| `look_apply` / `vfx_apply` / `decal_apply` / `ui_compose` / `material_apply` | spec の各節が **同じ純ロジック**（`resolveLook`・`resolveVfx`・`planDecal`・`composeUi`・`planPbr`）を呼ぶ。各ツールは単体でも Core に残る |
| `quality_gate` / `polish_audit` / `brief` | verify の実体。Brief の `budget`/`mustHave` を新チェックへ（§4.4.5） |
| `dx12_batch` | 低レベルの一括。apply の内部・Long tail |

#### 4.4.5 テンプレ / レシピ / ブリーフ・監査の拡充

- **レシピ**（`recipes/*.json`、`dx12_list_recipes {kind}`）: `room`（床壁天井・出入口・ライト自動）/ `corridor` / `arena` / `platformer_stage` / `fps_player` / `hud_minimal` … **パラメータ付きの仕様の断片**（`{recipe:"room", params:{size:[8,3,6], doors:1}}` → entities に展開）。既存の VFX 33・ルック 13・デカール 14・ステージテンプレ（`--template fps`）と同じ流儀で、**AI が 0 から座標を書かない**ための足場。
- **Brief の拡張**（`jev/brief.ts` の既存型に加算）: `genre` `mood` `palette` `references[]`（画像）`budget{fps,frameMsP95,draws,vramMB}` `mustHave[]`（「クリアできる」「ボスが居る」等）。`brief` は既にあるので**新チェックの閾値と判断材料に使う**（Jev 無しでもルールだけで動くことを維持、§8 Q3）。
- **監査の拡充**（`quality_gate` の `checks` に追加）: `errors`（新規エラー 0）/ `visual`（baseline 差分）/ `perf`（budget）/ `reachable` / `assetGap`（足りないモデルの検出 → `dx12_model_brief`）。各 check が `fix` 付き `blocking` を返す。

---

### 4.5 AI 自動テスト/品質検証（目的 ②）

**基本方針**: 既にある部品（決定論ステップ・`play_script`・`.playtest`・`golden.mjs`・`perf_stats`・`ciClient`）を **MCP から 1 コールで回せる形に整え、結果を機械可読の合否で返す**。新しい概念は増やさない。

#### 4.5.1 コンソール/スクリプトエラーの収集（リング + カーソル）

- エンジンの `Logger` にシンクを足し、**リングバッファ（4,096 件・単調増加の `seq`）**を持つ。`dx12_get_log {since?:seq, levels?:["error","warn"], grep?, limit?}` → `{entries:[{seq,t,level,src,msg}], nextCursor, dropped}`。現状の `get_log` は `dx12_engine.log` を CWD 相対で開くだけ（`ApplicationMcpEditor.cpp:166-186`）で、CWD が書けない場所だと空になる罠があるため、**リングを正**にする（ファイルは併存）。
- `errors` 集約: スクリプト（`get_script_errors` の live loadError）・シェーダーコンパイル失敗・D3D12 デバッグレイヤ・Lua 例外・`配置検査` ログを `since` 以降で束ねる。`quality_gate` の `errors` チェックは「**カーソル以降の新規 error = 0**」で合否。
- `dx12_play_script`/`run_playtests` は実行前後のカーソルを自動で記録し、`errorsDuringRun` を結果に含める。

#### 4.5.2 プレイテストの記録/再生の拡張（`dx12_playtest {op}`）

| op | 内容 | 現状との関係 |
|---|---|---|
| `record` | 直前のプレイを `.playtest` 化（人のプレイ→ゴールデンラン） | `record_playtest` |
| `run` | 1 本/全部/タグ指定を再生して突き合わせ | `run_playtests` |
| `list` `show` `delete` | 一覧・詳細・削除 | 新規 |
| `rebase` | ゴールデンランを撮り直す（理由必須・履歴に残す） | 新規（現状は手作業で再 record） |
| `suite` | `suites/<name>.json`（テスト名の列・タグ・リトライ・許容差）を回す | 新規 |

拡張点: ①**断言の追加**（`play_script` の `expect` を playtest にも: `near/yAbove/entityAlive/luaVar/logNoErrors/uiVisible/eventFired`）②**乱数の固定**（`seed`。エンジン RNG と Lua `math.random` の seed を設定する method を追加。再現性が無いテストは「ゆらぎ率」で扱う）③**チェックポイント撮影**（`at: 2.5s で決定論スクショ → baseline 比較`）④**フレーク対策**（`repeat:N` で N 回実行し `stability` を返す。1 回落ちて N-1 回通る→`flaky` として blocking にしない）⑤**成果物**を `<project>/.dx12/test-runs/<runId>/`（trace・スクショ・ログ切片・JUnit XML）へ保存し `artifacts:[path]` を返す。

#### 4.5.3 決定論スクショ差分と baseline 管理（`dx12_visual {op}`）

- 保存先 `<project>/.dx12/baselines/<scene>/<name>.png` + `baselines.json`（`{name, camera, hash, adapter, driver, engineVersion, renderSettingsHash, createdAt}`）。
- `op:"capture"`（baseline 作成）/ `"compare"`（現在を**決定論撮影**（`deterministic:true, settleFrames`。`AGENTS.md` 677-691）して比較）/ `"approve"`（現在を新 baseline に。`reason` 必須・履歴に残す・**AI が勝手に承認して合格を偽装しない**よう、`quality_gate` は `approve` 直後の run を「承認済み」と明示し人の確認対象にする）/ `"list"`。
- 比較は `tools/bench/golden.mjs` の **2 段判定**（画素 SHA-256 の完全一致 = `bitExact`／面積平均で縮めた画像の許容差 = 合否）を TS へ移植（pngjs は既に依存、`lookCompare.ts`/`contactSheet.ts` 流用）。返り値 `{pass, bitExact, diffRatio, maxDelta, diffBBox, diffImage, sideBySide}`。
- **baseline は adapter 名 + ドライバ版でキー付け**（別 GPU では比較しない=偽陽性を避ける。R10）。

#### 4.5.4 性能ゲート（`dx12_perf_gate`）

`budgets.json`（`<project>/.dx12/budgets.json`。`{fps:{min:60}, frameMsP95:{max:18.5}, gpuMs:{max:15.5}, cpuWorkMs:{max:5}, draws:{max:…}, vramMB:{max:…}, poses:[…]}`）に対し、`step_frames(180)` で収束 → `benchmark(600, uncap)` → `perf_stats(window:240)` を**姿勢ごと 3 回・中央値**で判定（`VIRTUAL_GEOMETRY_DESIGN.md:895-917` の手順そのまま）。返り値は指標ごとの `{value, limit, pass, margin}` と、`.dx12/perf-history.jsonl` に追記した履歴との比較（`regression:"+12% vs 直近5回中央値"`）。**`quality_gate` の `perf` チェックの実体**。VG の `vg_50m` 予算は同じ形式のプリセット（§4.7）。

#### 4.5.5 UI 自動テストの MCP 公開

- `UiTestHarness`（ImGuiTestEngine、`gui/UiTestHarness.h`）に MCP の口を付ける: `ui_test_list {category?}` / `ui_test_run {category|names, speed, deepOnly}`（**ジョブ化**。実行中は進捗、終了で `{tested, success, failed:[{name, log}], junit}`）。ハーネスは実行前にシーンを退避し完了後に復元する（`UiTestHarness.h:19-20`）ので `effect:"write_scene(自動復元)"` と表示。
- **AI が書く UI シナリオ**は、より軽い `dx12_imgui {op:"script", steps:[…], expect:[…]}`（`find` → `click` → 状態断言。`play_script` の UI 版）で足りる。ImGuiTestEngine 版と仮想入力（`--background`）の共存は **M10 で実機プローブ**（未検証。R11）。

#### 4.5.6 CI から `--headless` で回す（`ci_run` ジョブ）

`ciClient.ts` を「検査スイートランナー」に拡張（`ciClient.ts:399` の起動部を共用）:

```
node tools/mcp-server/ciClient.ts --launch <projectDir> --suite ci.json --report build/ci-report.json --junit build/ci-junit.xml
# ci.json: [{"check":"layout"},{"check":"errors"},{"check":"playtests","tags":["smoke"]},
#           {"check":"visual"},{"check":"perf","budget":"default"},{"check":"ui_tests","category":"basic"}]
```

空きポート自動選択・PID 指定 kill・終了コード 0/1 は既存の作法。**落とすのは blocking だけ**（警告で落とすと誰も直さない、`AGENTS.md:106-107`）。MCP 側は `dx12_job{kind:"ci_run"}` として同じスイートを起動できる。**GPU が無い CI では visual / perf を自動でスキップ**（`skipped:"no-gpu-adapter"`）し、GPU ランナー（RTX 機）では全部回す。現行の `.github/workflows/ci.yml` は TS の `npm test` のみ（`ci.yml:27-46`）で GPU 検査は走っていないため、**GPU ランナーの有無が未決事項**（§8 Q4）。

---

### 4.6 エディタ操作の全面公開（目的 ③）

**方針**: 「クリックで探し当てる」低レベル操作より前に、**コマンド表のコマンドを名前で実行**できる面を作る。`cmd::Execute(ctx, env, id)`（`EditorCommands.cpp:181`）は既に「メニュー・ショートカット・パレットの全経路が通る一点」なので、MCP をその 4 本目の入口にするだけで **約 81 コマンド（`kCommands` 32 + `window.*` 26 + `create.*` 23）が新規コード無しで**公開できる。新しいコマンドを表に足せば自動で MCP にも出る。

#### 4.6.1 新エンジン method 群（新規 `ApplicationMcpEditorUi.cpp`）

| method（MCP ツール） | 内容 | 実装の要点 |
|---|---|---|
| `editor_command_list`（`dx12_editor_command{op:"list"}`） | `{commands:[{id,label,labelEn,category,chord,scope,mode,enabled,checked,kind:"command"\|"window"\|"create",open?}]}`。`query`（`FuzzyMatch.h` を流用した曖昧検索）・`category`・`enabledOnly` で絞る | 3 つの表を走査。`IsEnabled`/`IsChecked`（`EditorCommands.cpp:150-177`）で**現在の可否**を付ける＝「今実行できるか」を AI が知れる |
| `editor_command_run`（`{op:"run", id, args?}`） | `Execute` を実行。**遅延応答**（`imgui_key` と同じ「2 フレーム待ってから返す」）で `{executed, enabled, effects:{windowsOpened, toast, mode, selection, entityCountDelta}}` | 無効（Play 中の Editor 専用コマンド等）は `E_MODE_CONFLICT` + `reason`。`create.*` は `args:{position?, name?}`。**ファイル選択ダイアログを開く `file.open`/`file.saveAs` は仮想入力中に出せない**（`MCP.md:677`）ので `E_UNSUPPORTED` + `fix:dx12_open_scene / dx12_save_scene` |
| `editor_state`（`{op:"state", sections?}`） | `{windows:[{id,title,open,slot,docked,rect}], dock:{nodes:[{id,axis,ratio,children,windows}]}, selection:[…], gizmo:{mode,space}, flyMode, view2D, paused, playing, modals:[{title,buttons}], toasts:[…], focusedWindow, palette:{open}}` | `tools::kAll` + `EditorContext` のフラグ + ImGui のドックノード走査 + ポップアップスタック。**「今モーダルが出ていて UI 操作が効かない」を AI が知る**最重要の読み取り |
| `editor_layout`（`{op:"layout", action:"get"\|"save"\|"load"\|"reset", name}`） | 名前付きレイアウト（`<project>/.dx12/layouts/<name>.ini`）。`--background` は `imgui.ini` を保存しない（`MCP.md:635`）ので**明示保存が必要** | `SaveIniSettingsToMemory` / `LoadIniSettingsFromMemory`、`view.resetLayout`（既存コマンド） |
| `editor_window`（`{op:"window", action:"open"\|"close"\|"focus"\|"dock"\|"float"\|"collapse", id, target?, rect?}`） | 窓の開閉（`window.*` と同じ）・フォーカス・ドック先の指定・浮動窓の位置/サイズ | ImGui `DockBuilder*` / `SetWindowPos/Size` を次フレームに適用。**`SetForegroundWindow` 等の OS 前面化は呼ばない**（ImGui 内のフォーカスのみ） |
| `editor_settings`（`{op:"settings", action:"get"\|"set", scope:"engine"\|"project", keys}`） | `settings.json`（`render_instancing` `render_clustered` …）とエンジン設定窓の項目 | 設定表（キー・型・範囲・`requiresRestart`）をマニフェスト同様に宣言。再起動が要る項目は `restartRequired:true` を返す |
| `editor_notify`（`{op:"notify", kind, message, seconds}`）/ `editor_toasts` | AI → 人へのトースト通知（`ctx.Notify`、`EditorContext.h:626`）。人に見えたエラー通知の読み取り | 「生成完了」「要確認」を人に静かに伝える（OS 通知は使わない） |
| `editor_select`（`{op:"select", entities\|names\|clear, focus?}`） | 複数選択・フォーカス（`select_entity` の拡張）。選択は `edit.*` コマンドの対象になる | `ctx.selectedEntities` |
| `editor_modal`（`{op:"modal", action:"get"\|"respond", button}`） | 開いているモーダルの読み取りと応答（ボタン名指定） | ImGui ポップアップ名・ボタンを列挙し、**仮想クリック**で応答 |

`dx12_editor_command` 1 本に `op` で束ねる（Core は 1 本）。**マニフェスト上は別 method**なので、`dx12_tool_describe` で `op` ごとの引数が引ける。

#### 4.6.2 `imgui_*` の運用ルール（既存ルールの明文化と自動化）

`docs/MCP.md` §4-18 / `AGENTS.md` の最重要ルールを**機械的に守らせる**:

1. **梯子の順**: (i) `dx12_editor_command`（名前で実行）→ (ii) 専用 method（`select_entity`, `set_component` …）→ (iii) `dx12_imgui`（`find` で矩形 → `pointer`/`key`）。(iii) は (i)(ii) に無い**ウィジェット操作**（Inspector の値欄・ツリー・スライダ）だけ。
2. **背景起動 + 仮想入力**が前提。`dx12_imgui`/`editor_*` は呼び出し時に `imgui_virtual_input` の状態を読み、OFF なら `E_VIRTUAL_INPUT_OFF`（`fix`: `{op:"mode",enable:true}`）。ON にしたモードは MCP が 5 秒切れたら自動 OFF（人の脱出口、`MCP.md:671`）なので、長い操作は `dx12_imgui` が**ハートビートで維持**する。
3. **不変の証跡**: `dx12_imgui`/`editor_command_run` は前後で `osCursor` / `isForegroundWindow` を読み（既存の読み取り専用値）、**返り値の `safety:{osCursorDelta:[0,0], foregroundChanged:false}`** に載せる。0 でなければ `E_SAFETY_VIOLATION` を返して以降の操作を止める（ありえないはずの事故を検知するための安全弁）。
4. **禁止事項は文書だけでなくツール面から消す**: computer-use / PowerShell の前面化を促す説明・ツールは作らない。`dx12_guide{topic:"editor"}` に禁止事項を固定文で載せる。

#### 4.6.3 テスト

- ctest `McpEditorCommandCoverage`: 3 つの表の**全 id**が `editor_command_list` に出て、モック ctx で `Execute` が実行できる（`editor_ux_test.cpp` の流儀）。新コマンドが表に入れば自動で網羅される。
- 実機 E2E（`--background`、`tools/bench/lib/engine.mjs` 流の起動）: `window.postProcess` を実行 → `editor_state` で `open:true` → `create.box` → エンティティ数 +1 → `edit.undo` で戻る → 全工程で `osCursorDelta = 0`。

---

### 4.7 仮想ジオメトリ / UE 取込のツール（目的 ④）

`VIRTUAL_GEOMETRY_DESIGN.md` の段階（P0〜P10、`:700-721`）に**紐づけ、その段階のマージ条件に MCP を含める**（設計書自身が「MCP のスキーマ変更はドリフトテストと `docs/MCP.md` を同時更新」を全段階共通条件にしている、`:704`）。マニフェスト駆動なら、エンジン側の追加は**`McpDefine` + meta だけで MCP に現れる**ので、VG 側の作業から TS 改修が消える。

| VG 段階 | 必要な MCP 面 | 経路 | 備考 |
|---|---|---|---|
| **P0 仕様確定 + スタブ** | `dx12_vgeo_info {path}`（ヘッダ・セクション表・DEBUG_JSON を読む。エンジン不要） | **TS で `.vgeo` ヘッダ/セクション表をパース**（§3.2 の 512 B ヘッダ・CRC）。C++ `VgeoReader` と同じ golden ファイル（`tests/data/vgeo/`）で TS/C++ の**読み結果一致テスト** | 返り値: `{version, triCount(source), clusterCount, levels, pages, bytes, quantBits, materials, proxyTris, sourceHash, cooker}` |
| **P1 オフライン cooker** | `dx12_vg_cook {src, out?, proxyTris?, threads?, params?, dryRun?}` = **ジョブ**（§4.3.5）。`dryRun` は入力三角形数から `.vgeo` サイズ/所要時間を見積もる（§3.9 の式） | TS が `vgeocook.exe` を spawn（自分の PID のみ kill）、標準出力の `PROGRESS stage n/m` を進捗化。完了時に `.vgeo` の DEBUG_JSON（レベル別クラスタ数・誤差・ロック頂点率・cook 時間・決定論ハッシュ）を `result` に。同入力で 2 回撃って**バイト同一**（`sourceHash`/出力 CRC）を確認する `verify:"deterministic"` | 1000 万 tri ≤ 5 分（設計書の合否）に合わせ、ジョブは `timeoutMs` を 30 分既定 |
| **P2 ランタイム読込 + カリング** | `set/get_virtual_geometry`（設計書 P2 の予定）→ **`render_setting` group の `target:"virtual_geometry"` として自動で Core の `dx12_set/get_render_settings` に出る** / `perf_stats.virtualGeometry` は `dx12_get_perf` に通す / `dx12_vg_dump {maxClusters}`（選択クラスタ ID 一覧。小シーンのみ）→ `dx12_vg {op:"dump"}` | エンジン method + meta のみ | `dx12_get_perf` の返り値スキーマに `virtualGeometry{enabled,instances,sourceTrisInFrustum,visibleClusters,nodesVisited,levelHistogram[],overflow,vramMB,phase2Clusters}` を meta の `returns` に宣言 |
| **P3 メッシュシェーダ描画** | `render_debug` の VG モード（`vgCluster/vgLod/vgTri/vgOverdraw/vgCoverage`）→ `dx12_capture{view:"debug", mode}` の `mode` 列挙は**マニフェストから**（TS に列挙を書かない） | エンジン method の enum を meta に | 穴画素率 `vgCoverage` は `dx12_get_perf` にも数値で出す（ゲート用） |
| **P4 resolve + 既存統合**（**最初の合格判定: 5000 万 tri / 60 fps**） | `dx12_perf_gate` の `vg_50m` プリセット（設計書 §5.2 の合否表: fps ≥ 60・1% low ≥ 50・frameMs p95 ≤ 18.5・GPU ≤ 15.5・VG GPU ≤ 10.0・`sourceTrisInFrustum` 50〜80M・`drawnTris` 0.3〜12M・`overflow` 0・穴 ≤ 0.01%・VRAM ≤ 1500MB・デバッグレイヤ警告 0） | §4.5.4 の汎用ゲート + `budgets/vg_50m.json` | 設計書の `tools/bench/vg_bench.mjs` は**このゲートの CLI ラッパ**にする（二重実装しない） |
| **P5 ストリーミング** | `dx12_vg_stream {op:"state"\|"set"\|"wait_resident"}`: `state`=`{residentPages, pendingRequests, streamedMB, holePixels, budgetMB, evictions, dependencyViolations}`、`set`=`{enabled, budgetMB, pin[]}`、`wait_resident`=`pendingRequests==0` まで待つ（ベンチの手順そのもの、進捗通知つき） | エンジン method + meta（`state` は read、`set` は write_setting） | `dependencyViolations = 0` を perf_gate の必須項目に |
| **P6 UE アセット cook（C#）** | `dx12_ue_scan {source, filter}`（メッシュ一覧・Nanite 有無・三角形数。**P6 冒頭 3 日のスパイクの可否判定の材料**）/ `dx12_ue_import {source, filter, out, cook?:true}` = **ジョブ**（`tools/ue-cook`（`dotnet`）→ VGSRC → `vgeocook` → `.vgeo` + `textures/*.dds` → assets へ登録） | TS が子プロセスを順に spawn。**UE 由来の配置（P6 (c)「材質 + 配置」）を scene spec の `entities` に変換**して `dx12_apply_scene_spec` に渡せる | Dreamcore Dead Mall（CUE4Parse・`.usmap`）の実績（memory `dreamcore-dead-mall`）を再利用 |
| **P10 エディタ / MCP / 配布** | VG 設定窓の開閉と Inspector 項目は `editor_command`（`window.*` に窓を足せば自動）、`ui_test` の診断項目追加 | 表に足すだけ | 配布ビルドで `.vgeo` を読んで起動する検査は `ci_run` のスイート項目に |

**安全策**: cook / import は**ディスク容量の事前チェック**（見積サイズ ×1.5）・**同時実行 1 本**・出力先は `assets/` 相対のみ（既存のパス規約 `MCP.md:899`）・失敗時は部分出力を削除。`.vgeo` の破損は `dx12_vgeo_info` が CRC で即検出。

---

## 5. 段階計画

**原則**: 各段階は ①単体でマージできる（既存の 220 ツールと全テストを壊さない）②合否を機械で判定できる ③別エージェントへ並列に振れる単位。工数は「1 人（または 1 エージェント）が集中した実働日」。**全段階共通のマージ条件**: `cd tools/mcp-server && npm run test:offline`（と `npm test`）緑、C++ を触る段階はエンジン `ctest` 全緑（現状 49 本）、既存の旧ツール名の呼び出しが変わらない（§6 の alias スナップショットテスト）。

**最初の 3 段階（M1 メタ層+マニフェスト → M2 エラー構造化 → M3 Core 統合+動的登録）が完了すれば、不満 (a)(b)(c) の大半が解消する**（M0 は計測のみ）。

### 5.1 一覧（1 行 / 段階）

| ID | 段階 | 目的 | 依存 | 工数(日) | C++ | レーン |
|---|---|---|---|---:|:-:|---|
| **M0** | プローブと基準線 | 現状の選択率・サイズを数値化。list_changed 等の未確認事項を実機で確定 | なし | 2 | × | 直列 |
| **M1** | マニフェスト + shell 層 | エンジン登録表を唯一の真実に。`dx12_tool_search/describe/call/doctor/guide` + `instructions`。旧 220 は無傷 | M0 | 5 | ○ | 直列（先頭） |
| **M2** | エラー構造化 + 事前検証 + 自己診断 | (c) を解消。コード体系・fix/didYouMean・中央検証・doctor・再接続 | M1 | 4 | ○ | A |
| **M3** | Core 統合 + 動的登録 + 名前規約 | (a)(b)。220→約 35、consolidated 生成、toolset 切替、lint、ホットリロード | M1, M2 | 6 | × | A |
| **M4** | 文書/後始末の自動化 | docs・README・guides・Lua API を生成、`finalize` 1 コマンド | M1（M3 後が望ましい） | 3 | 小 | A |
| **M5** | 副作用の安全性 | effect 分類・冪等一般化・dryRun・file journal・guarded ゲート | M1, M2 | 5 | ○ | B |
| **M6** | 進捗・キャンセル・ジョブ | progress 中間行・cancel・`dx12_job` | M1（M2） | 4 | ○ | B |
| **M7** | エディタ公開 1 | `editor_command` list/run・`editor_state`・notify・select | M1 | 4 | ○ | C |
| **M8** | エディタ公開 2 | layout/window/settings/modal・`imgui script`・安全証跡 | M7 | 5 | ○ | C |
| **M9** | 自動テスト 1 | ログリング・`get_log`・errors 集約・視覚回帰 baseline | M1, M2 | 6 | ○ | D |
| **M10** | 自動テスト 2 | playtest 拡張・perf_gate・UI テスト公開・CI スイート | M6, M9 | 7 | ○ | D |
| **M11** | 宣言的シーン生成 | `dx12_apply_scene_spec`（validate/plan/apply/verify） | M2, M5 | 8 | 小 | E |
| **M12** | レシピ + Brief/監査の拡充 | `recipes/*`・Brief 拡張・quality_gate の新チェック | M11（M9, M10） | 5 | × | E |
| **M13** | VG-a | `vgeo_info`・`vg_cook` ジョブ（VG の P0〜P1 に同期） | M6、VG P0/P1 | 4 | × | F |
| **M14** | VG-b | VG 統計/ストリーミング/`vg_50m` ゲート/UE 取込（P2〜P6 に同期） | M13, M10、VG P2〜P6 | 5 | 小 | F |
| | **合計** | | | **73** | | |

**クリティカルパス**: M0(2) → M1(5) → M2(4) → M3(6) = **17 日で (a)(b)(c) の大半が解消**。全体は M1 の後に 6 レーンが並行でき、最長は M2(4) → M5(5) → M11(8) → M12(5) = 22 日 → **M0〜M12 で約 29 実働日**（3〜4 エージェントで暦 5〜6 週間）。M13/M14 は VG 本体の進捗（P1〜P6）に合わせて随時。

```
M0 ─> M1 ─┬─> M2 ─┬─> M3 ─> M4
          │       ├─> M5 ─> M11 ─> M12
          │       └─> M9 ─┐
          ├─> M6 ────────┴─> M10 ──────> (M12 の新チェック)
          ├─> M7 ─> M8
          └─> M13 (VG P0/P1) ─> M14 (VG P2〜P6, M10 の perf_gate を使う)
```

**衝突回避**: `index.ts`（6,907 行）を複数エージェントが同時に触るとマージが壊れるので、**M1 の最終日に「`index.ts` のカテゴリ別モジュール分割（コードの移動のみ・挙動不変）」を入れ、その後に各レーンへ分岐**する。C++ は新規ファイルに分ける（`ApplicationMcpEditorUi.cpp` / `ApplicationMcpLog.cpp` / `ApplicationMcpManifest.cpp` …）。**ディスパッチャ本体（`ApplicationMcp.cpp:110-316`）を触る M1 → M2 → M5 は直列**にする。新規 `.cpp` の追加は `src/core/CMakeLists.txt` と `tests/CMakeLists.txt` の `DX12E_MCP_SOURCES`（`MCP.md:972`）を更新する。

### 5.2 各段階の詳細（(a) 成果物 / (b) 依存 / (c) 工数 / (d) 合否基準）

#### M0 プローブと基準線（2 日）
- **(a)** `tools/mcp-server/eval/discovery_tasks.json`（付録 A の 30 タスク×言い換え 2）と `eval/run_offline.ts`（現行 220 本の名前+説明に対する素朴な検索の recall@3 を測る）、`eval/run_live.ts`（`claude -p` か Agent SDK で dx12 サーバのみ接続し、最初に呼んだ dx12 ツールを記録。**起動フラグ・SDK の API は実装時に確認**）、`probes/`（① 実行中にツールを追加する stub サーバ ② `structuredContent` と text の扱い ③ 中断時の `notifications/cancelled` ④ progress の表示/idle 延長）、`legacy-tools.snapshot.json`（現行 220 の名前・引数キー・annotations。`tools/list` = 408,644 B の基準も記録）。結果を §3 の「未確認事項」へ書き戻す。
- **(b)** なし。
- **(c)** 2 日。
- **(d)** 基準値が数値で残る（offline recall@3・live 選択率・tools/list サイズ）。**4 つのプローブに「Claude Code v◯◯ で ○/×」の結論が付き**、動的登録（§4.2）・progress/cancel（§4.3.5）の既定 ON/OFF が決まる。

#### M1 マニフェスト + shell 層（5 日。うち最終日は `index.ts` の機械分割）
- **(a)** エンジン: `McpMeta`/`McpEffect`/`McpParam` と `McpDefine` の meta 付き多重定義（meta 無しは paramSpec から自動導出）、`describe_mcp_manifest`、`ping.manifestHash`、Core 関連約 60 method の meta 記入（残りは自動導出のまま）。TS: `manifest.ts`（取得・snapshot・索引）、`search.ts`（bigram+BM25）、shell 5 本（`tool_search/describe/call` は完成、`doctor` は基礎版、`guide` は `AGENTS.md` の機械分割）、サーバ `instructions`（§4.1.1）、旧名 alias 表、`defineTool`/`composites/` の器、`index.ts` のカテゴリ別分割（移動のみ）。
- **(b)** M0。
- **(c)** 5 日。
- **(d)** ① 分割前後で `tools/list`（full モード）の名前集合とスキーマ JSON が**一致**（snapshot 比較）。② エンジン `m_mcpMethods` の全件（174）が `describe_mcp_manifest` に出て、mock と実エンジンの両方で `dx12_call` から撃てる（ctest `McpManifestTests`: `manifest.count == m_mcpMethods.size()`）。③ **再起動不要テスト**: mock エンジンが実行中に method を追加（`manifestHash` が変わる）→ **MCP サーバプロセスを再起動せず**、次の `dx12_tool_describe`/`dx12_call` がその method を扱える（stdio クライアントのハーネスで自動）。④ offline **recall@3 ≥ 90%**（M0 基準より改善）。⑤ `instructions` ≤ 2,048 字、`tools/list` サイズが M0 基準を**超えない**。⑥ `test:offline`・`schemaDrift`・`paramGuard`・ctest 緑。

#### M2 エラー構造化 + 事前検証 + 自己診断（4 日）
- **(a)** `errors.ts`（エンベロープ・コード表）、エンジン: `McpErr` 拡張・`McpError` に `fix/didYouMean/details`・ディスパッチャの中央検証（meta の required/enum/range/type）・近い名前の提案（entity/asset/scene/component）・未知 method 専用コード・`expectGeneration` → `E_STALE_SCENE`、TS: `dx12_doctor` 完全版、接続再試行、マニフェスト由来の timeout、遅延結果ジャーナル、標準語への語調統一、`dx12_launch_engine`。
- **(b)** M1。
- **(c)** 4 日。
- **(d)** **エラー再現テスト** `errors.cases.json`（**40 ケース**: 不正 enum / 未知・欠落・型違い・範囲外の引数 / 存在しない entity・asset・scene の打ち間違い / 古い世代 / モード衝突 / 未知 method / エンジン停止（ポートが閉じている）/ ポートファイルが古い / 別クライアントが占有 / 応答なし（mock）/ タイムアウト後の遅延応答）が、期待コード・**空でない `fix`**・タイポ系で **`didYouMean[0]` が正解**を返す。**自己修復率**: 壊れた呼び出し 20 件に対し「`fix[0]` か `didYouMean[0]` を機械的に適用して再送」が **≥ 80% 成功**（LLM 不要の機械テスト）。メッセージ中の方言語尾（`かもしれん|してくれ|やで|ちゃう` など）が 0（lint）。

#### M3 Core 統合 + 動的登録 + 名前規約（6 日）
- **(a)** マニフェストの `group/op` から生成する consolidated ツール（`dx12_get/set_render_settings`（26→2）・`dx12_edit_terrain`+`dx12_query_terrain`（12→2）・`dx12_capture`（8→1）・`dx12_imgui`（5→1）・`dx12_get_perf`（2→1）・`dx12_list_entities`（5→1）ほか）、Core registry、`DX12_MCP_TOOLSET=core\|full\|legacy`、`_meta`（`alwaysLoad` / `requiresUserInteraction`）、annotations の `effect` 導出、`outputSchema` 削除、`tools/list` の決定的順序、composites のホットリロードと list_changed（開発時 `DX12_MCP_DEV=1`）、`tool-surface.test.ts`（§4.1.7 の lint）、`dx12_call_guarded`。
- **(b)** M1, M2。（Core 28 のうち M7/M9/M11 で作る `editor_command`・`get_log`・`apply_scene_spec` は**その段階で追加**。M3 時点の Core は既存機能で構成できるもの。）
- **(c)** 6 日。
- **(d)** ① `core` モードの `tools/list` が **≤ 40 本かつ ≤ 120 KB**（現行 408,644 B の 30% 以下）。② 旧 220 名の**すべて**が `dx12_call` で解決し引数互換（alias スナップショット 220/220）、`full` モードは従来と**同一**。③ lint 違反 0（名前規約・動詞と annotations の整合・説明長・引数名・outputSchema なし・shell alwaysLoad 5 本）。④ **ライブ選択率 ≥ 85% かつ M0 基準から −5pt 未満**（未達なら §7 R2 の撤退）。⑤ 既存テスト全緑。⑥ M0 のプローブ結果に従い動的登録の既定を決めて記録。
- **★ここまでで (a)(b)(c) の大半が解消**（AI は 35 本 + 検索で目的に届き、新 method は再起動なしで使え、エラーは `fix` を読んで直せる）。

#### M4 文書/後始末の自動化（3 日）
- **(a)** `npm run gen:docs`（`docs/MCP_TOOLS.md`・README の表・`guides/*.md` をマニフェストと `AGENTS.md` から生成、`--check` は CI）、`lua_api.json`（Lua API の単一ソース）→ `McpLuaApi()` / `API_REFERENCE.md` §2 表 / `docs/index.html` 該当表の生成と `--check`、`npm run finalize`（gen:docs・lua-api・schemaDrift・test:offline・manifest snapshot 更新・publish の差分確認を 1 本に。**push は人の確認が要る操作として分離**）。
- **(b)** M1（M3 後が望ましい）。
- **(c)** 3 日。
- **(d)** マニフェストを変えて docs を更新しないと CI が赤になる（意図的な差分で確認）。ctest: 「sol2 で実際に登録された Lua 名 ⊆ `McpLuaApi` 辞書」を検査（現状の差分を allowlist に固定し**漸減**させる）。`finalize` 1 コマンドで §6.5 の手作業が全部走る。

#### M5 副作用の安全性（5 日）
- **(a)** 全 method の `effect` 記入（lint で未記入 0）、ディスパッチャでの**冪等キー一般化**、`withTransaction` の共通化、`dryRun:"txn"`（tx 中は自動保存を保留）、**file journal**（`McpWriteFile`。`create_lua_component` / `create_shader` / `scene_write` / decal atlas / `import_asset` を対応）、guarded ゲート（エンジン側）、`dx12_call{dryRun, txn, idempotency_key}` と timeout 時の自動再送。
- **(b)** M1, M2。
- **(c)** 5 日。
- **(d)**（自動: ctest + mock + 実機ヘッドレス）① マニフェストを走査して、**`write_scene`/`write_setting` の全 method に `dryRun:true` を投げてもシーン JSON のハッシュが不変**。② 最初の応答をタイムアウト後まで遅延させ自動再送 → **エンティティが 1 つだけ**・`idempotentReplay:true`。③ atomic な batch（`create_lua_component` → 失敗する op）の後に**ファイルが残らない**（既存ファイルは復元）。④ tx 中は 2 秒アイドルでも自動保存が走らない（偽の時計で ctest）。⑤ `guarded` の method を `guarded:true` 無しで生 TCP から撃つと拒否される。⑥ effect 未記入の method 0。

#### M6 進捗・キャンセル・ジョブ（4 日）
- **(a)** エンジン: `McpProgress` 中間行・`cancel` method（step_frames / render_debug / deterministic 撮影 / diagnose / terrain_erode / imgui drag / benchmark）。TS: `notifications/progress`、`extra.signal` → cancel、`dx12_job`（start/status/cancel/list、`.dx12/jobs/*.json` に永続、外部プロセスジョブの汎用実装: spawn・PID 指定 kill・標準出力の進捗パース）。
- **(b)** M1（遅延結果ジャーナルは M2）。
- **(c)** 4 日。
- **(d)** ① `progressToken` 付きの `step_frames 600` が**進捗通知 ≥ 3 回**（mock クライアント）。② abort で `step_frames` が **1 秒以内に止まり**、進んだフレーム数 < 要求（実エンジン・ヘッドレス）。③ `dx12_job`: `start` が **500 ms 未満**で返り、進捗が単調増加、`cancel` は**自分が起動した PID だけ**を落とす（ダミー子プロセスで検証）、MCP 呼び出しのタイムアウトを跨いでも結果が取れる。④ cancel 不可の method は `cancellable:false` を返す。

#### M7 エディタ公開 1（4 日）
- **(a)** `ApplicationMcpEditorUi.cpp`: `editor_command_list/run`、`editor_state`、`editor_notify/toasts`、`editor_select`、`E_MODAL_OPEN`。Core `dx12_editor_command`。
- **(b)** M1（M2 が望ましい）。
- **(c)** 4 日。
- **(d)** ctest `McpEditorCommandCoverage`（3 表の**全 id**が list に出て、モック ctx で実行できる。現状の合計 81 が表と一緒に増減）。**`--background` 実機 E2E**: `window.postProcess` を実行 → `editor_state` で `open:true` → `create.box` → エンティティ数 +1 → `edit.undo` で戻る。**全工程で `osCursorDelta = 0`**。Play 中の `file.new` は `E_MODE_CONFLICT` + `reason`。

#### M8 エディタ公開 2（5 日）
- **(a)** `editor_layout`（save/load/reset）・`editor_window`（dock/float/focus/collapse）・`editor_settings`（`settings.json` と設定窓、`restartRequired`）・`editor_modal`・`dx12_imgui{op:"script"}`（steps+expect）・`safety` 証跡・`dx12_guide{editor}`。
- **(b)** M7。
- **(c)** 5 日。
- **(d)** レイアウト保存 → `view.resetLayout` → 読込で `editor_state.dock` の**ノード構造ハッシュが保存時と一致**。`editor_settings` の set→get 往復・`restartRequired` の明示。`editor_modal` で in-app ダイアログ（`file.saveAs`）を閉じる。`imgui script` で Inspector の Position 欄に値を入れ `get_entity` で確認（`AGENTS.md` の型）。**全て `osCursorDelta = 0`**。

#### M9 自動テスト 1: エラー収集と視覚回帰（6 日）
- **(a)** `Logger` のリングシンク（4,096 件・`seq`）・`dx12_get_log{since,levels,grep}`・errors 集約・`quality_gate` の `errors` チェック、`dx12_visual{op:"capture"\|"compare"\|"approve"\|"list"}`・baseline 保管（adapter/driver キー）・`golden.mjs` の 2 段判定の TS 移植。
- **(b)** M1, M2。
- **(c)** 6 日。
- **(d)** ① 意図的に出した error 10 件を `since` カーソルで**漏れなく・重複なく**取得。CWD が書けない場所で起動しても取得できる。② 故意に壊した Lua・シェーダで `errors` チェックが blocking + `fix`。③ **同一設定 2 回の compare が `bitExact:true`**（決定論撮影・実機）、色を 1 箇所変えると fail + `diffBBox`、別 adapter の baseline は `skipped`、`approve` は `reason` 必須。④ `tests/golden/` 既存 4 枚に対する TS 版の合否が `golden.mjs` と一致（パリティ）。

#### M10 自動テスト 2: プレイテスト拡張・性能ゲート・UI テスト・CI（7 日）
- **(a)** `dx12_playtest{op}`（suite/rebase/list/show/delete）・`expect` 拡張・`set_random_seed`・チェックポイント撮影・`repeat`/flaky 判定・成果物保存、`dx12_perf_gate`（budgets・履歴・姿勢×3 回の中央値）、`ui_test_list/run`（ジョブ化）と仮想入力との共存プローブ、`ciClient --suite/--report/--junit`、Actions ワークフロー例。
- **(b)** M6, M9。
- **(c)** 7 日。
- **(d)** ① 既存 fixture プロジェクトで同じ playtest を seed 固定で 10 回 → `stability 100%`。ジャンプ力を故意に変えると `run` が fail + reasons。② `perf_gate`: 既知シーン（`stress_5000`）で予算内なら pass、予算を絞ると fail + `margin`、履歴に人工的に +12% を入れると `regression` を検出。③ `ui_test_run` 全件の pass 数が**パネルからの手動実行と一致**（JUnit）、`--background` との共存結果を記録。④ `ciClient --suite ci.json` が終了コード 0/1 + JUnit を出し、**GPU 無し環境では visual/perf を `skipped` で報告**。⑤ ctest 緑。

#### M11 宣言的シーン生成（8 日）
- **(a)** scene spec v1 の JSON Schema、`dx12_apply_scene_spec`（validate/plan/apply/verify）、`specId` による差分、JSON Pointer 付き `issues` + `specPatch`、既存純ロジックの内包（§4.4.4）、fixture 仕様 10 本。
- **(b)** M2, M5（M9 の errors チェックは任意）。
- **(c)** 8 日。
- **(d)** ① fixture 10 本が validate → plan → apply → verify で pass（`layout.errors == 0`）。② **冪等**: 同じ仕様を再 apply → `create:0, unchanged:N` かつシーン JSON ハッシュ不変。③ 失敗系 20 種（未知アセット・参照切れ・`id` 重複・cm/m 疑い…）が正しい `issues[].path` を返し、**`specPatch` を機械適用すると ≥ 80% が pass に転じる**。④ 途中失敗で**変更ゼロ**（ハッシュ）。⑤ 200 体の仕様の apply が「`create_entity` を 200 回」より **≥ 5 倍速い**（実測）。

#### M12 レシピ + Brief/監査の拡充（5 日）
- **(a)** `recipes/*.json` を 8 種（room / corridor / arena / platformer_stage / fps_player / hud_minimal / outdoor_terrain / horror_hall）、`dx12_list_recipes`、Brief 拡張（genre/mood/palette/budget/mustHave）とバリデータ、`quality_gate` の新チェック配線（`errors` `visual` `perf` `reachable` `assetGap`）、`dx12_guide{build_scene}`。
- **(b)** M11（新チェックは M9, M10）。
- **(c)** 5 日。
- **(d)** 各レシピをパラメータ 3 通りで apply して `quality_gate.pass`。`brief.budget` を絞ると `perf` が blocking を返す。`mustHave` 未達が `suggestions` に出る。discovery 評価に「作る」系 6 タスクを足して recall@3 ≥ 90%。**AI エンドツーエンド（live）**: 「ブリーフ 1 つ → 部屋 1 つ → `quality_gate` pass」が 3 回中 2 回以上成功（試行数が少ないので**参考指標**）。

#### M13 VG-a（4 日。VG の P0〜P1 に同期）
- **(a)** `dx12_vgeo_info`（TS で `.vgeo` を読む）、`dx12_vg_cook` ジョブ（spawn・進捗パース・`dryRun` 見積・決定論検証）、ディスク容量チェック。
- **(b)** M6、VG P0（形式確定）。cook ジョブは P1 の CLI 完成後。
- **(c)** 4 日。
- **(d)** golden `.vgeo` 2 種で TS と C++ の読み取り結果が**フィールド完全一致**、破損 10 種（VG P0 の `VgeoFormatTests` と同じ fixture）を検出。cook ジョブ: 小アセットで start → 進捗 → result、**同入力 2 回でバイト同一**、`dryRun` の見積サイズが実測 ±30%。

#### M14 VG-b（5 日。VG の P2〜P6 に同期、段階ごとに分割着手）
- **(a)** `perf_stats.virtualGeometry` の返り値宣言、`vg_dump`、`render_debug` VG モード列挙のマニフェスト化、`perf_gate` の `vg_50m` プリセット（設計書 §5.2 の合否表）、`dx12_vg_stream{state,set,wait_resident}`、`ue_scan`/`ue_import` ジョブ、UE 配置 → scene spec の変換。
- **(b)** M13, M10、VG の各段階成果。
- **(c)** 5 日（段階分割）。
- **(d)** VG 各段階のマージ条件に「MCP 面がマニフェストに現れ `dx12_tool_describe` で引ける」を追加（設計書の共通条件 `:704` と同型）。`set_virtual_geometry{enabled:false}` で**ビット一致**（既定 OFF の回帰）。`vg_50m` プリセットが VG P4 の合否表と**同じ判定**を返す。`ue_scan` が Dead Mall 代表 10 アセットを列挙。

---

## 6. 互換性・移行

### 6.1 既存ツール名・引数を壊さない

- **旧 220 名は恒久的にサポート**（`legacy-tools.snapshot.json` を M0 で固定し、`alias` 表を生成）。解決順: Core 名 → 旧名 alias → エンジン method 名。旧名を渡すと旧引数のまま動き、**返り値の形も旧形式のまま**（新エンベロープは Core と `dx12_call` のみ）。`dx12_tool_search` は旧名でもヒットする。
- **ツールセットの段階的切替**（環境変数 `DX12_MCP_TOOLSET`）:

| 値 | 内容 | 既定になる時期 |
|---|---|---|
| `full` | 旧 220 + Core + shell をすべて登録 | **M1〜M3 の間の既定**（何も壊れない） |
| `core` | shell 5 + Core 28 + `dx12_call_guarded` + `dx12_batch`（≒ 35 本） | M3 の合格（§5 (d)）後の**次のリリース**で既定に |
| `legacy` | 現行とバイト同一（固定したい利用者・回帰用） | いつでも指定可 |

- **許可リストへの影響**: 利用者が `mcp__dx12-engine__dx12_set_ssao` のように旧名を allow している場合、`core` では旧名がツールとして出ず `dx12_call` 経由になる。移行案内を `dx12_guide{errors}` と README に載せ、**M3 の時点で「`dx12_get_*` を一括 allow する書き方」**（§4.1.2）を提示。`full` を選べば従来どおり。
- **引数の互換**: consolidated ツールは旧形（例 `dx12_set_ssao{intensity:1}` は `dx12_set_render_settings{target:"ssao",values:{intensity:1}}` に**アダプタ**で変換）。旧引数名を消さない。

### 6.2 既存テストの維持

| テスト | 扱い |
|---|---|
| `npm run test:offline` / `npm test`（`package.json`、約 40 本） | **常に緑**が全段階のマージ条件。新テスト（`manifest.test.ts` `errors.test.ts` `tool-surface.test.ts` `discovery.test.ts` `journal.test.ts` …）を追記 |
| `schemaDrift.test.ts` | M1〜M2: 現状のまま維持（エンジン `.cpp` ⇔ 旧 zod）。M3 で生成対象になったツールは「マニフェスト ⇔ エンジン」の検査へ移し、**エンジン側 ctest（`mcp_param_spec_test` の拡張）が「meta.params ⇔ ハンドラ本文のキー」を守る**（TS は `.cpp` をテキスト解析しなくなる）。移行が終わるまで両方が走る |
| `paramGuard.test.ts` | 未知キー検査はマニフェスト駆動に一般化（既存のテストケースはそのまま通す） |
| `test.ts`（mock エンジン） | framing/相関/タイムアウトは維持。`tools/list`・ツール登録の検証（現状は省略、`test.ts:11-12`）を **`tool-surface.test.ts`** として追加 |
| エンジン ctest（49 本） | 全緑を維持。`McpManifestTests` `McpEditorCommandCoverage` を追加 |
| `tools/bench/golden.mjs` | 存続（TS 版 `dx12_visual` と結果パリティを取る。M9） |

### 6.3 配布リポジトリ（`dx12-mcp`）との整合

MCP サーバはエンジンと別配布（`CLAUDE.md:460`）。**版ずれは前提**: `manifest.snapshot.json`（TS リポジトリ同梱）+ 実行時の `manifestHash`/`protocol` で扱う（§4.2.3）。`publish.ps1` は既にサブフォルダを丸ごとミラーする（`publish.ps1:37-46`）ので `composites/ recipes/ guides/ eval/` は自動で乗る。**push は外部リポジトリへの公開なので自動化せず、`finalize` の最後に「差分確認 → 人の承認」を置く**。

### 6.4 `lua-api-checklist` との整合

現行の運用（memory `lua-api-checklist`）: ①`McpLuaApi()` 辞書 ②`API_REFERENCE.md`（§2 + §3）③`SCRIPTING.md` ④`docs/index.html`、補完（`GetCompletions`）は動的列挙で自動。**M4 で ①②④の表を `lua_api.json` から生成**（③は手書きの短い例なので生成しない）し、ctest で「登録済み Lua 名 ⊆ 辞書」を検査する。補完は現状どおり自動。**手作業は「`lua_api.json` に 1 項目足す + SCRIPTING.md の例を書く」の 2 つに減る**。

### 6.5 「MCP を触るたびの後始末」の自動化（`npm run finalize`）

| 現在の手作業（memory / CLAUDE.md） | 自動化 |
|---|---|
| `docs/MCP.md` に新ツールを手書き（`ApplicationInternal.h:956`） | `gen:docs`（マニフェスト → `MCP_TOOLS.md`・README 表） |
| TS スキーマにキーを足し忘れない（N21） | マニフェスト生成（M3 以降。それまでは `schemaDrift`） |
| Lua API を作ったら辞書 + リファレンス 3 点 | `gen:lua-api` + ctest（§6.4） |
| `publish.ps1` を走らせる（`CLAUDE.md:461`） | `finalize` が差分を表示し、承認後に実行 |
| 新 method の `timeout` 表・`readOnly` 判定への追加 | マニフェストの `timeoutMs`/`effect` から導出（手書き表を廃止） |
| `AGENTS.md` のツール索引の更新 | 生成部分（索引）と手書き部分（罠・手順）を分離 |

### 6.6 Codex など他クライアント

`instructions` / `dx12_tool_search` / `dx12_guide` / `dx12_call` は**標準 MCP の範囲**で動く。`_meta["anthropic/*"]`（alwaysLoad・requiresUserInteraction）は Claude Code 専用の拡張で、他クライアントは無視するだけ。Codex がツールを遅延読み込みするか・`list_changed` に対応するかは**未調査**なので、Codex 利用者には `DX12_MCP_TOOLSET=full` を既定の案内にする（§8 Q1）。

---

## 7. リスクと撤退条件

| ID | リスク | 検知 | 緩和 | 撤退条件 / 代替 |
|---|---|---|---|---|
| R1 | **Claude Code が `list_changed` を ToolSearch の索引に反映しない**（#66084）／更新した description が古いまま（#97369） | M0 プローブ（実行中追加ツールが ToolSearch で見つかり呼べるか） | 主経路を `dx12_call`/`dx12_tool_describe`（定義が固定の shell）にしてあり、動的登録は補助。Core の description は固定 | プローブが ×: **動的登録を既定 OFF**（`DX12_MCP_DEV` のみ）にして shell 経由に一本化。再起動不要の要件は `dx12_call` で満たす |
| R2 | **メタ層で AI の選択精度が落ちる**（`dx12_call` の名前/引数を間違える、Core 以外に辿り着かない） | M3 のライブ選択率、日常運用の失敗ログ（`dx12_call` の `E_UNKNOWN_TOOL`/`E_BAD_*` 率） | Core 28 が 9 割をカバー、`callTemplate` をそのまま撃てる、事前検証 + `didYouMean` + `fix`、`instructions` の手順 | ライブ選択率が基準より **5pt 超低下**、または `dx12_call` のエラー率が 15% 超: Core を 60 本に増やす → それでも駄目なら `full` を既定に戻す（旧 220 は無傷なので即日戻せる） |
| R3 | **`dx12_call` が権限の抜け穴**（許可した瞬間に何でも通る） | lint（guarded の網羅）、生 TCP テスト | effect 分類・`dx12_call_guarded`・エンジン側ディスパッチャの guarded ゲート（多層防御）・`requiresUserInteraction` | guarded の漏れが 1 件でも出たら M5 に戻して修正（それまで `core` を既定にしない） |
| R4 | **マニフェスト移行が 174 method で終わらない** | meta 記入率（CI 表示） | meta 無しは paramSpec から自動導出して動く。Core 関連 60 本を M1 で、残りは触ったときに記入。lint の閾値を段階的に上げる（M5 で effect 100%） | 記入率が伸びなくても機能は劣化しない（自動導出）。M5 の effect 100% だけは必達 |
| R5 | **TS とエンジンの版ずれ**（配布リポジトリ別配布） | `manifestHash`/`protocol` 不一致 | `E_ENGINE_TOO_OLD`・snapshot・`dx12_doctor` | 互換を保てない変更（`protocol` 更新）は 1 リリース前から警告 |
| R6 | **単一クライアント制約**（`listen(…,1)`）で doctor の診断や並列テストが衝突 | doctor の `E_ENGINE_BUSY`（connect は通るが応答が無い） | 診断は connect のみ（ping を送らない）。テストは別ポートのヘッドレス（8850〜、既存の作法） | 複数クライアントが必須になった場合は TS 側の多重化（MCP セッション多重）を別課題化 |
| R7 | **MCP spec 2026-07-28 / Claude Code v2 ランタイムへの移行**（initialize 廃止・`server/discover`・tasks 拡張） | Claude Code の既定変更（changelog）、SDK 2.0 の安定化 | SDK 1.29 に固定し、stdio では既定で新リビジョンを問い合わせない現状（§3 (vii)）を確認。トランスポート層を薄く保つ | Claude Code が stdio でも新リビジョンを既定にした時点で SDK 移行を別計画化 |
| R8 | **progress / cancel が Claude Code で効かない**（cancel は公式記載なし） | M0 プローブ | 主役は `dx12_job` とタイムアウト。progress は付加価値 | 効かなければ progress/cancel は実装を最小化し、ジョブ API に集中 |
| R9 | **structuredContent の扱いが不明** | M0 プローブ | 本文 JSON を唯一の正にし続ける（outputSchema 削除） | 該当なし（依存しない設計） |
| R10 | **視覚回帰の偽陽性**（GPU/ドライバ差・TAA/ノイズ） | baseline 比較の不安定さ、`bitExact` の頻繁な失敗 | 決定論撮影・adapter+driver でキー付け・許容差 2 段判定・`approve` は理由必須 | 偽陽性が運用に耐えない: 画素比較をやめ、`perceive`（ID パスの構造指標）と `look_compare` の統計指標だけをゲートにする |
| R11 | **UiTestHarness と仮想入力（`--background`）が共存しない** | M10 のプローブ | `dx12_imgui{op:"script"}`（軽量 DSL）で代替 | 共存不可: UI 自動テストは `--ui-tests-run-all`（CI 専用の通常起動）だけにし MCP 公開は見送る |
| R12 | **エディタコマンドの競合**（モーダル中・ドラッグ中に実行） | `E_MODAL_OPEN`、`editor_state.modals` | 実行前に必ず状態確認、無効時は理由つきで拒否 | — |
| R13 | **ホットリロードの事故**（配布版で意図せず有効・メモリ増） | — | `DX12_MCP_DEV=1` の時だけ有効。既定 OFF | 問題が出たら機能ごと撤去（動的 `import()` を使わないだけで通常運用に影響なし） |
| R14 | **工数超過**（73 日・15 段階） | 各段階の実績日数 | M3 で (a)(b)(c) が解消するので、**M3 の後に優先度を再評価**して不要な段階を切れる | M3 合格後、ユーザーが (a)(b)(c) に満足なら M5 以降は必要性で選択 |
| R15 | **`index.ts` 分割のマージ衝突** | 分割中の他ブランチの変更 | M1 最終日に凍結窓を設け、移動のみ・挙動不変（tools/list の同一性テスト）で 1 日で終える | 衝突が多発したら分割を M3 に後ろ倒し（M1 の (d)① は分割前のままでも成立） |

---

## 8. 未決事項（ユーザーに聞くべき点。推奨案つき）

1. **既定ツールセットの切替時期と、旧ツール名の許可リストへの影響** — M3 の合格後の次のリリースで `core`（≒ 35 本）を既定にし、旧 220 名は `dx12_call` 経由で恒久サポートする案でよいか。`mcp__dx12-engine__dx12_*` のような旧名の allow を利用者が持っていると `core` では効かなくなる（`full` で回避可）。**推奨: 上記どおり。Codex を使うなら Codex だけ `full` を既定にする。**
2. **`dx12_call` の権限の切り方** — `dx12_call`（read/write/runtime）と `dx12_call_guarded`（git push・eval_lua・delete_asset・build_game … 毎回確認）の 2 本に分ける案。`eval_lua` は AI が最もよく使う道具の 1 つだが任意コード実行なので guarded に入れる。**推奨: 2 本分割。`eval_lua` は guarded、ただし利用者が `dx12_call_guarded` を明示的に allow すれば毎回確認を外せる運用（`requiresUserInteraction` を付けるのは push/delete_asset/build_game だけ）。**
3. **判断段（Jev）への依存** — `quality_gate` の拡充（errors/visual/perf/reachable）は**ルールだけで完結**させ、Jev（外部 API・鍵必須・入力を外部送信、`jev/client.ts:17`）は「意図に照らした keep 判断」の任意の追加層のままにしてよいか。**推奨: ルールで完結、Jev は任意（現状維持）。新しい必須依存にはしない。**
4. **視覚回帰 baseline の承認者と CI の GPU** — baseline は adapter+driver 名でキー付けして「この開発機（RTX 5060）」を基準にし、`approve` は理由必須で履歴に残す運用でよいか。CI（現状 `windows-latest` は `npm test` のみ、`ci.yml:27-46`）に GPU ランナーを用意するか、無ければ visual/perf は手元/自宅ランナー限定にするか。**推奨: approve は人が確認する運用（AI が単独で承認しない）、GPU ランナーが無い間は visual/perf を `skipped` で報告し、リリース前に手元で `ci_run` を回す。**
5. **着手順序と VG の扱い** — M0〜M3（(a)(b)(c) の解消）を先行し、M13/M14（VG）は VG 本体の P1〜P6 の進捗に合わせて随時、という順序でよいか。**推奨: 上記。M4（自動化）は M3 の直後、M7 以降（エディタ・自動テスト・シーン生成）は M3 合格後にユーザーの体感で優先度を決める。**

---

## 付録 A. 発見性評価の代表 30 タスク（M0 で `eval/discovery_tasks.json` に固定）

各タスクは日本語の依頼文（+言い換え 1 通り）と、許容する最初のツール。**内訳: Core / shell で直接届く 28 + 長尾を `dx12_tool_search` 経由で届く 2**。M5（git など guarded）・M13（`.vgeo` 情報）・M12（作る系）の各段階で拡張枠として追加する。

| # | 依頼（要旨） | 期待ツール |
|---|---|---|
| 1 | 今開いているシーンの中身を一覧したい | `dx12_list_entities` |
| 2 | Player の位置とコンポーネントを見たい | `dx12_get_entity` |
| 3 | 床用の大きな箱を置いて名前を付ける | `dx12_create_entity` |
| 4 | glb モデルを (0,0,5) に置く | `dx12_spawn_model` |
| 5 | 壁の位置とスケールを変える | `dx12_set_transform` |
| 6 | ポイントライトの強さを上げる | `dx12_set_component` |
| 7 | ブルームとトーンマップを調整する | `dx12_set_render_settings` |
| 8 | 今の SSAO 設定を読む | `dx12_get_render_settings` |
| 9 | 夕暮れのホラー調に一括で寄せる | `dx12_look_apply` |
| 10 | 木材の PBR テクスチャ 4 点を貼る | `dx12_material_apply` |
| 11 | 松明の炎を置く | `dx12_vfx_apply` |
| 12 | 山の地形を作って雪/岩/草を割り当てる | `dx12_edit_terrain` |
| 13 | 回転する Lua スクリプトを作って貼る | `dx12_create_lua_component` |
| 14 | タイトル画面の UI を組む | `dx12_ui_compose` |
| 15 | 木を 100 本散らして置く | `dx12_apply_scene_spec` |
| 16 | 部屋を一式（床・壁・ライト）作って検証まで | `dx12_apply_scene_spec` |
| 17 | 人が見る最終画を撮る | `dx12_capture` |
| 18 | ある視点から撮る | `dx12_capture` |
| 19 | エンジンのログ/スクリプトエラーを確認する | `dx12_get_log` |
| 20 | FPS とボトルネックを測る | `dx12_get_perf` |
| 21 | Play して止める | `dx12_play` / `dx12_stop` |
| 22 | 入力の台本でゴールまで行けるか確かめる | `dx12_play_script` |
| 23 | 保存済みのプレイテストを全部回す | `dx12_run_playtests` |
| 24 | 作業の区切りで総合検査 | `dx12_quality_gate` |
| 25 | 「ポストプロセス」ウィンドウを開く / コマンド一覧を見る | `dx12_editor_command` |
| 26 | Inspector の値欄をクリックして入力する | `dx12_imgui` |
| 27 | シーンを開く / 保存する | `dx12_open_scene` / `dx12_save_scene` |
| 28 | エンジンに繋がらない原因を知りたい | `dx12_doctor` |
| 29 | ナビメッシュを焼いてゴールへ行けるか見る | `dx12_tool_search` → `dx12_check_reachable`（長尾） |
| 30 | 直前の AI の編集を元に戻す | `dx12_tool_search` → `dx12_call`{undo}（長尾） |

## 付録 B. 本書の数値の再現方法

- `tools/list` の実測: `tools/mcp-server` で `node index.ts` を stdio 起動し、`initialize`（protocolVersion 2025-06-18）→ `notifications/initialized` → `tools/list` を送って応答 JSON のバイト数・ツール数を数える（エンジン・エディタは起動しない。遅延接続のため）。結果: 220 本 / 408,644 B、description 73,115 字・inputSchema 112,796 字・outputSchema 44,899 字、capabilities は `{"tools":{"listChanged":true}}` のみ。
- エンジン method 数: `src/core/mcp/ApplicationMcp*.cpp` の `McpDefine("…")` を数え `"a|b"` を展開して 174。
- カテゴリ集計（§2）: `index.ts` の `reg(`/`regRaw(` ブロックを機械的に抽出し、手動でカテゴリを割り当てた（220/220 割当済み・重複なし）。
