# MCP / AI Bridge — 完全リファレンス

起動中の DX12 エディタを Claude Code / Codex から操作するための MCP(Model Context Protocol)連携ガイド。
AI がシーンを読み・エンティティを生成し・コンポーネントを設定し・Lua を貼り・Play/Stop まで回せる。

> ### ★ AI がエディタ UI を操作・撮影するときは【必ず】仮想入力モード + `--background`
>
> エディタ(ImGui の画面)を AI が触るために、実マウス / 実キーボード / フォーカスを操作するスクリプト
> (`SendInput` / `mouse_event` / `SetCursorPos` / `SetForegroundWindow` / computer-use)を**使ってはいけない**。
> 人がカーソルを奪われ、PC を使えなくなる。代わりに:
>
> 1. エディタを **`DX12Engine.exe --background --project <dir>`** で起動する(窓を手前に出さない静かな起動。仮想入力モードも含意)。
>    起動済みなら `dx12_imgui_virtual_input {enable:true}`。
> 2. UI の操作は `dx12_imgui_pointer` / `dx12_imgui_key`、狙う場所は `dx12_imgui_find`、画面は `dx12_imgui_screenshot`(§4-18)。
>
> 仮想入力モード中、エンジンは OS のカーソル・フォーカス・前面ウィンドウに一切触れない。

---

## 0. まずここから: shell 5 本(ツールの探し方・呼び方・診断・ガイド)

ツールは 200 本を超える。AI が名前を推測せず、**常時ロードの shell 5 本**で探して撃つ(設計は `docs/MCP_ENHANCEMENT_DESIGN.md` §4.1.4)。
旧 220 ツールは**名前・引数・成功時の返り値とも従来のまま**登録されている(`DX12_MCP_SURFACE` で見せ方を切り替えられる。§0-5)。
**主力 28 本だけを `tools/list` に出す `core` 面**(M3。既定は `full` のまま)もある。

| ツール | 使いどころ | 返り値 |
|---|---|---|
| `dx12_tool_search {query, category?, effect?, tier?, limit?}` | 目的の操作の名前が分からないとき。日本語/英語の自然文・旧ツール名・エンジン method 名で検索(決定論。同義語辞書つき) | `{hits:[{name, tier, kind, summary, category, effect, mode, example, score}], total, hint, catalog}`。0 件なら `didYouMean` / `categories` |
| `dx12_tool_describe {name, target?}` | 引数(型・必須・enum・範囲)・副作用・タイムアウト・例・次の一手・起こりうるエラー・旧名を見る。`target` は引数が多いツールの絞り込み | `{name, kind, effect, mode, timeoutMs, dryRun, params[], examples[], next[], errors[], legacy, callTemplate:{name,args}}` |
| `dx12_call {name, args?, dryRun?, confirm?, timeoutMs?, idempotency_key?}` | 任意のツール/エンジン method を、送信前にスキーマ検証してから実行。旧名でもエンジン method 名でもよい | 成功 `{ok:true, result, meta:{tool, method, effect, tookMs, warnings?, lateResults?, undoEntry?}}` / 失敗は構造化エラー(下) |
| `dx12_doctor {deep?}` | エンジンに繋がらない/おかしいときの自己診断。最初に撃つ入口 | `{ok, summary, engine, ports, process, versions, tsServer, recentErrors, issues[{code, severity, message, fix[]}], log?, next}` |
| `dx12_guide {topic?}` | 目的別の最短手順・危険操作の注意・仮想入力の運用ルール(Markdown) | トピック一覧 / 本文。`build_scene` `test` `lighting` `ui` `editor` `safety` `errors` `perf` `engine_dev` |

サーバの `instructions`(2,048 字以内)にも同じ使い分けと最重要ルール(人の PC 操作を奪わない・`--background`)を載せてある。

```
dx12_tool_search {query:"ブルームを調整"}            → dx12_set_post_process ほか
dx12_tool_describe {name:"dx12_set_post_process", target:"bloom"}   → bloom 関連の引数を説明つきで
dx12_call {name:"dx12_set_post_process", args:{bloomOn:true, bloom:0.8}}
dx12_call {name:"dx12_delete_entity", args:{name:"Wall_01"}, dryRun:true}   → 実行せず対象・破壊性・Undo 可否だけ返す
```

### 0-1. 再起動不要でエンジンの新しい method を使う

エンジンの登録表(`McpDefine` + `McpMeta`)が唯一の真実。エンジンに method を足して再ビルド・再起動すると、`ping.manifestHash` が変わり、
**MCP サーバ(Node)を再起動しなくても**、次の `dx12_tool_search` / `dx12_tool_describe` / `dx12_call` が `describe_mcp_manifest` を取り直して新 method を扱う
(TS ラッパも Claude Code の再起動も要らない。これが**主経路**)。
Core にも載せたい method だけ `McpMeta.expose = "core"` を付けると、MCP サーバが `notifications/tools/list_changed` を送って `tools/list` にも増やす(補助。
**Claude Code がこの通知を反映するかは未確認**なので、これに依存しない。`DX12_MCP_LIST_CHANGED=0` で止められる)。最短手順は §0-6、全体は `dx12_guide {topic:"engine_dev"}`(= `tools/mcp-server/guides/engine_dev.md`)。

### 0-2. 構造化エラー(`dx12_call` の失敗 / 旧ツールの失敗に付く JSON)

```jsonc
{"ok":false,
 "error":"…",                      // 人が読む 1 文(エンジンの error をそのまま)
 "error_code":"E_BAD_ENUM",        // 文字列コード(下表)
 "engineCode":2,                   // 旧来の数値 error_code(あれば)
 "cause":"…", "retryable":false,
 "didYouMean":["night"],           // 打ち間違いの近い候補(ツール名/引数キー/enum 値/エンティティ名/シーン/アセット)
 "validValues":["day","dusk","night", …],
 "fix":[{"tool":"dx12_apply_lighting_preset","args":{"preset":"night"},"why":"…"}],   // そのまま dx12_call へ渡せる。shell コマンドは {command, why}
 "details":{…}, "docs":"dx12_tool_describe {name:'…'}"}
```

旧ツールを MCP から**直接**呼んだ場合は、従来の日本語本文(1 ブロック目)は変えず、**2 ブロック目**に同じ JSON が付く。
引数の誤りは**エンジンへ送る前に**検出する(旧ツールは zod スキーマ、TS ラッパの無い method はマニフェストの params で検証)。

| error_code | 旧 | 意味 | 既定の fix |
|---|---:|---|---|
| `E_ENGINE_UNREACHABLE` | — | エンジンに繋がらない(ポート閉鎖/プロセス無し/切断) | `dx12_doctor`、`Start-Process … --background` |
| `E_ENGINE_BUSY` | — | 接続は通るが応答が無い(別クライアントが単一ブリッジを保持) | 他セッションを閉じる(doctor が判定) |
| `E_ENGINE_TIMEOUT` | — | 期限内に応答が来ない。**エンジンは処理を続けている可能性**(`details.engineResponsive`、遅れて届いた結果は次の `meta.lateResults`) | `dx12_ping` → 結果確認 → 撃ち直し |
| `E_ENGINE_TOO_OLD` | — | 旧ツールが呼ぶ method をエンジンが持たない / マニフェスト無しの古いエンジン | エンジンを更新して再起動 |
| `E_UNKNOWN_TOOL` | 8 | ツール/method 名が無い | `didYouMean`(編集距離+別名+検索)、`dx12_tool_search` |
| `E_UNKNOWN_PARAM` `E_MISSING_PARAM` `E_BAD_TYPE` `E_BAD_ENUM` `E_OUT_OF_RANGE` | 2 | 引数不正 | `fix[0].args`(機械的に直した引数)/ `validValues` |
| `E_NOT_FOUND_ENTITY` `E_NOT_FOUND_ASSET` `E_NOT_FOUND_SCENE` `E_NOT_FOUND_COMPONENT` `E_NOT_FOUND_COMMAND` | 1 / 6 | 対象が無い(近い名前を最大 5 件。`E_NOT_FOUND_COMMAND` はエディタのコマンド id) | `didYouMean` を入れた撃ち直し |
| `E_STALE_SCENE` | 4 | `expectGeneration` が古い | `dx12_list_entities` |
| `E_MODE_CONFLICT` | 3 | Editor/Playing の不一致・トランザクション中の禁止 method | `dx12_stop` など(`thenRetry:true` は撃ってから元の呼び出しを再送) |
| `E_VIRTUAL_INPUT_OFF` `E_MODAL_OPEN` | 3 / 13 | 仮想入力が OFF / モーダルが開いている | `dx12_imgui_virtual_input {enable:true}` |
| `E_UNSUPPORTED` | 10 | 環境が非対応(再送は無駄) | 代替の案内 |
| `E_GUARDED` | 11 | guarded な操作に `confirm:true` が無い | dryRun → 承認 → `confirm:true` |
| `E_VALIDATION_FAILED` `E_FILE_IO` `E_CANCELLED` `E_SAFETY_VIOLATION` `E_INTERNAL` | 2 / 14 / 12 / — / 7 | 宣言的入力の検証失敗 / ファイル I/O / 中断 / 仮想入力の安全違反 / 内部エラー | 各ガイド(`dx12_guide {topic:"errors"}`) |
| `E_NOT_FOUND` `E_INVALID_PARAM` | 1 / 2 | 種類を特定できなかった旧経路 | メッセージと hint を読む |
| `E_FLEET_LIMIT` `E_FLEET_RESOURCE` | — | 専用エンジンが全体で上限(既定 3 台)/ 空き VRAM・RAM が下限未満(§0-7) | `fix` の自分の idle なエンジンの `dx12_engine_stop`。他人のエンジンは止めずユーザーに確認 |
| `E_FLEET_VISIBLE_DENIED` `E_FLEET_READONLY` | — | `visible` は既定で拒否 / 読み取り専用で繋いだエンジンへ書き込み系を送った | `mode:"background"` / `dx12_engine_launch`(自分専用) |
| `E_FLEET_NOT_FOUND` `E_FLEET_NOT_OWNER` `E_FLEET_PROJECT_IN_USE` | — | engine が無い / 他人のエンジン / 同じプロジェクトを別のエンジンが使用中 | `didYouMean`・`dx12_engine_list` |
| `E_FLEET_BUILD_IN_PROGRESS` `E_FLEET_LAUNCH_FAILED` `E_FLEET_EXE_MISSING` `E_FLEET_DISABLED` | — | exe の元がビルド中 / 起動失敗・無応答 / exe が無い / フリート無効 | ビルド完了を待つ・`details.logTail`・`tools\build.ps1` |
| `E_JOB_NOT_FOUND` `E_JOB_NOT_FINISHED` `E_JOB_NOT_OWNER` | — | ジョブ id が無い(または GC 済み)/ まだ終わっていない / 他のセッションの生きたジョブ | `dx12_job_list` / `dx12_job_status {id, waitSec:30}` / `force:true`(承認を得て) |
| `E_JOB_TOOL_MISSING` `E_JOB_FAILED` `E_JOB_TIMEOUT` `E_JOB_INTERRUPTED` `E_JOB_RUNNER_LOST` `E_JOB_DISABLED` | — | build.ps1・vgeo_cook・ctest・exe が無い / 処理の失敗(`summary` に errors・failedTests)/ `timeoutSec` 超過 / 起動したサーバの終了で中断 / runner が結果を残さず消えた / ジョブ API 無効 | `fix`・`dx12_job_logs`・`timeoutSec` を延ばす・同じ引数で再起動 |
| `E_IDEMPOTENCY_CONFLICT` `E_IDEMPOTENCY_IN_FLIGHT` | 2 / 9 | 同じ冪等キーで別の要求 / 前回の要求がまだ処理中 | 別のキーにする / 少し待って同じ要求を再送(`dx12_call` は自動で再送する) |

エンジン側の加算フィールド(`error_name` など)は §12-3。`docs` の語調は標準語・簡潔に統一(方言・命令口調は `errors.test.ts` の lint が見張る)。

### 0-3. dryRun / guarded / 警告(`dx12_call`)

- `dryRun:true`: 読み取り系はそのまま実行(`dryRun:"ignored(read-only)"`)。`look_apply` / `vfx_apply` / `decal_apply` / `sequence_author` / `organize_scene` は各ツールの native dryRun(計算結果を返す)。
  **エンジンが対応する method(マニフェストの `dryRun:"preview"`。18 件。§13-4)は、エンジンが「実際に何が起こるか」(対象と件数・破壊性・書くファイルと上書きか・`willFail`)を返す**(`dryRunMode:"engine"`。副作用ゼロ)。
  それ以外の書き込み系は**実行せず**、対象の存在(`get_entity` で確認)・破壊性・Undo 可否・ファイルを書くか、を `preview` に返す(非対応の範囲は `supported` に明示)。`dx12_batch` の dryRun は op ごとにエンジンのプレビューを引く(`dryRunMode:"engine-per-op"`)。
- **guarded**(`git_*` の書き込み系 7 種 / `eval_lua` / `delete_asset` / `build_game` / `net_launch_test_client`。`git_status` / `git_branches` は read): `full` / `shell` 面では `confirm:true` が無いと `E_GUARDED`。
  **`core` 面では `dx12_call` に `confirm:true` を付けても通らず**、`dx12_call_guarded`(`_meta["anthropic/requiresUserInteraction"]:true` = 毎回ユーザーが承認)から実行する。
  `dx12_batch` の op に guarded な method が混じっていたら、**全ての面で**(M5 で full / legacy にも拡張)1 つも実行せず `E_GUARDED`(batch はエンジン method 直叩きでゲートを素通りできるため)。
  **エンジン側にも最終関門がある(M5。§13-2)**: guarded な method は有効な `confirm_token`(`guard_token` で得る 1 回限りのトークン)が無いと**エンジンが拒否する**(生 TCP・`dx12_batch` の素通りも塞がる)。TS 側の `E_GUARDED` はその前段。旧ツールを直接呼ぶ場合はクライアントの権限設定(名前ベース)が効く。
- **冪等キー(M5)**: `dx12_call {idempotency_key}`(別名 `idempotencyKey`)は write 系の全 method / ツールで、同じキーの再送に前回の結果(`idempotentReplay:true`)を返して再実行しない(§6・§13-3)。ラッパの無い method と、`idempotency_key` を宣言した旧ツール(create_entity など)はキーをそのままエンジンへ渡す。それ以外の旧ツール / 合成ツール(`dx12_batch` など)は、呼び出しの文脈にキーを置き、中でエンジンへ撃つ write 系に**サブキー `<key>:<method>:<引数の sha1 先頭 8 桁>:<出現順>`** を付ける(同じ引数の繰り返しでも衝突せず、再送しても完了済みの部分は二重実行されない)。**キーを省略しても**、エンジン method に 1:1 の write 系は `auto-…` を自動採番し、`E_ENGINE_TIMEOUT` / `E_IDEMPOTENCY_IN_FLIGHT` のときは**同じキーで最大 3 回自動再送**する(エンジンが応答するまで待つ。`meta.autoRetried`)。合成ツールは自動採番しない。
- 未保存の変更を消す操作(`open_scene` / `new_scene` / `open_project`)は、`sceneDirty:true` のとき `meta.warnings` で事前に警告する。
- タイムアウト後に届いた応答は捨てずに保持し、次の `dx12_call` の `meta.lateResults` に載せる。切断後の次の呼び出しで自動再接続(接続失敗は 0.3 / 0.6 / 1.2 秒で再試行)し、再接続した事実(entityId の失効)を `meta.warnings` に載せる。

### 0-4. 環境変数

| 変数 | 内容 |
|---|---|
| `DX12_MCP_SURFACE` | ツール面(§0-5)。`full`(既定: shell 5 本 + 旧 220 本 + パストレーサー 3 + 仮想ジオメトリ 2 + フリート 6 + ジョブ 6 + エディタ操作 5。現状互換)/ `core`(shell 5 + フリート 2 + ジョブ 3 + Core 28 + `dx12_batch` + `dx12_call_guarded` = 40 本)/ `shell`(shell 5 本だけ)/ `legacy`(旧 220 本だけ・instructions 無し・outputSchema 有り。M0 と同一の回帰基準) |
| `DX12_MCP_TOOLSET` | `DX12_MCP_SURFACE` の旧名(互換)。`SURFACE` が有効ならそちらが優先。`full` / `legacy` / `shell`(設計書どおり `core` も可) |
| `DX12_MCP_LIST_CHANGED` | `0` / `false` / `off` で、マニフェストの `expose:"core"` による動的登録(`tools/list` の差し替えと `list_changed` 送出)を止める。既定は有効。止めても `dx12_tool_describe` / `dx12_call` では新 method が使える |
| `DX12_MCP_PORT` / `DX12_MCP_HOST` | 接続先(従来どおり) |
| `DX12_MCP_CONNECT_BACKOFF_MS` / `DX12_MCP_PORT_FILE` | テスト用(再試行間隔 / ポートファイルの場所) |
| `DX12_FLEET_DIR` / `DX12_FLEET_MAX` / `DX12_FLEET_IDLE_MIN` / `DX12_FLEET_MIN_FREE_VRAM_MB` / `DX12_FLEET_MIN_FREE_RAM_MB` / `DX12_FLEET_PORT_RANGE` / `DX12_FLEET_BUILD_DIR` | 専用エンジン(フリート)の設定。既定は置き場 `%LOCALAPPDATA%\UnoEngine\fleet`・上限 3 台・アイドル 10 分・空き VRAM 2048 MB / RAM 3072 MB 未満で拒否・ポート `8860-8899`。詳細は §0-7 |
| `DX12_MCP_ALLOW_VISIBLE` | `1` で `dx12_engine_launch {mode:"visible"}`(窓を画面に出す起動)を許可する。呼び出しの `confirm:true` も別に要る。既定は拒否(実マウス・フォーカスを奪い得るため) |
| `DX12_FLEET_AUTOLAUNCH` / `DX12_FLEET_DISABLE` | `1` で「束縛も従来の接続先も無いとき、最初の呼び出しで専用エンジンを自動起動」/ フリートのツールと監視を止める |
| `DX12_FLEET_ENGINE_CMD` / `DX12_FLEET_FAKE_RESOURCES` / `DX12_FLEET_DISCOVER_PORTS` / `DX12_DOCTOR_PORTS` | **テスト用**(偽エンジンの起動コマンド / 資源の観測値の差し替え / discover と doctor が見るポート) |
| `DX12_MCP_DEV_PROBE`(**エンジン側**) | `1` で動的登録の実機確認用ダミー method `dev_probe`(`expose:"core"`)をエンジンが登録する。通常起動では存在しない |

### 0-5. ツール面(surface)3 モードと Core

`tools/list` に何を出すかを環境変数 `DX12_MCP_SURFACE` で選ぶ(MCP 設定の `env` に書く)。**既定は `full`(現状互換)**。`core` を既定にするかは M3 合格後の別判断。
どの面でも旧 220 名は `dx12_call` の別名として**恒久サポート**(名前・引数・返り値の形は不変)。

| 面 | `tools/list` | 本数 / サイズ(実測) | 使いどころ |
|---|---|---:|---|
| `full`(既定) | shell 5 + 旧 220(情報の無い共通 `outputSchema` を削っただけ) | 225 本 / 350,762 B(M0 408,638 B 比 85.8%) | 従来どおり。許可リストに旧名を書いている人 |
| `core` | shell 5 + **フリート 2** + **ジョブ 3** + Core 28(**エディタ操作 2 を含む**)+ `dx12_batch` + `dx12_call_guarded` | **40 本(上限ちょうど)** | 通常の AI 作業。長尾は `dx12_tool_search` → `dx12_call` |
| `shell` | shell 5 | 5 本 / 6,708 B | 最小(全部 `dx12_call` 経由) |
| `legacy` | 旧 220 のみ | 220 本 / 408,638 B | 回帰基準(M0 と同一) |

**Core 28 本 + フリート 2 + ジョブ 3(選定は設計書 §4.1.5 と付録 A の使用頻度。M6 で `dx12_run_playtests` を、M7 で `dx12_play_script` と `dx12_engine_list` を、M11 で `dx12_scene_write` を長尾へ)**

| # | Core ツール | 中身 |
|---|---|---|
| 1-2 | `dx12_list_entities` `dx12_get_entity` | 旧ツールのまま |
| 3-4 | `dx12_get_render_settings` / `dx12_set_render_settings` | **統合(28 → 2)**: `target` = post_process / ssao / ssr / ssgi / taa / volumetric_fog / shadow_pcss / dxr / contact_shadow / occlusion / depth_prepass / normal_filter / render_scale / scene_settings。set は `values{…}`(旧 `dx12_set_<target>` の引数)。get は target 省略で 14 target をまとめて返す |
| 5-6 | `dx12_get_log` `dx12_get_script_errors` | 旧ツールのまま |
| 7 | `dx12_get_perf` | **統合(2 → 1)**: `mode` = snapshot(`perf_stats`)/ benchmark(`benchmark`)。`frames` / `uncap` があれば benchmark |
| 8 | `dx12_capture` | **統合(8 → 1)**: `view` = final(既定)/ scene / game / ui / debug / texture / from / focus |
| 9 | `dx12_apply_scene_spec` | **宣言的シーン生成(M11。§0-10)**: 仕様 JSON を差分適用 → 自動検証 → 失敗は specPatch。`dx12_scene_write`(M3 ではその代役として Core に居た)は M11 で長尾へ。`dx12_scene_spec_export` は長尾 |
| 10-14 | `dx12_create_entity` `dx12_spawn_model` `dx12_set_transform` `dx12_set_component` `dx12_delete_entity` | 旧ツールのまま |
| 15-17 | `dx12_look_apply` `dx12_material_apply` `dx12_vfx_apply` | 旧ツールのまま |
| 18 | `dx12_edit_terrain` | **統合(10 → 1)**: `op` = create / generate / sculpt / erode / paint / autopaint / set_layers / sculpt_create / sculpt_make_editable / sculpt_brush |
| 19-20 | `dx12_create_lua_component` `dx12_ui_compose` | 旧ツールのまま |
| 21-23 | `dx12_play` `dx12_stop` `dx12_quality_gate` | 旧ツールのまま(`dx12_run_playtests` は M6 で長尾へ。同じ回帰確認を進捗つきで `dx12_job_start {kind:"playtest"}` が担う。**`dx12_play_script` は M7 で長尾へ**: 入力の台本つき検証は Core の `dx12_play` / `dx12_stop` + `dx12_get_log` で足りる場面が大半で頻度が低く、M10 で `dx12_playtest` へ統合する予定の枠。`dx12_call {name:"dx12_play_script"}` で名前・引数・返り値そのまま使える) |
| 26 | `dx12_imgui` | **統合(5 → 1)**: `op` = virtual_input / find / pointer / key / screenshot(仮想入力のみ。実マウス・実キーボードには触れない)。コマンド表で足りる操作は下の `dx12_editor_command` を先に |
| +2 | `dx12_editor_command` `dx12_editor_state` | **エディタ操作(M7。§0-9)**。`dx12_editor_command {op:list\|run\|describe}` = コマンド表(メニュー・ショートカット・パレットと同じ)を id で実行 / `dx12_editor_state {scope}` = 選択・窓・レイアウト・モーダル・モード・Undo・通知・性能(読み取り専用)。`dx12_editor_notify` / `dx12_editor_select` / `dx12_editor_modal` は長尾 |
| 27-28 | `dx12_open_scene` `dx12_save_scene` | 旧ツールのまま |
| +2 | `dx12_engine_launch` `dx12_engine_stop` | **専用エンジン(フリート。§0-7)**。shell の直後に並ぶ。**M6 でジョブ 3 本を入れるため `attach` / `refresh` / `use` を、M7 でエディタ操作 2 本を入れるため `list` を長尾(`dx12_call`)へ移した**(`attach` は頻度が最も低い / `refresh` は build ジョブの `refreshEngines:true` が「ビルド → 更新」を担う / `use` は M3 から長尾 / `list` は `launch` の返り値と `dx12_doctor` の fleet 欄で代替できる) |
| +3 | `dx12_job_start` `dx12_job_status` `dx12_job_cancel` | **ジョブ API(§0-8)**。`dx12_job_list` / `dx12_job_result` / `dx12_job_logs` は長尾(`dx12_call`) |
| +1 | `dx12_batch` | 旧ツールのまま(core / shell 面では guarded な op を拒否) |
| +2 | `dx12_call_guarded`(core 面のみ) | guarded 専用の実行口。毎回ユーザー承認(`requiresUserInteraction`) |

- 「旧ツールのまま」の Core は、**名前・inputSchema・annotations が旧ツールと同一**で、説明文だけ Core テンプレ(1 文の要約 / 使う / 使わない / 副作用 / 注意 / 次。600 字以内)に差し替わる(core 面のみ。旧文は `dx12_tool_describe` の `description` に残る)。
- 統合ツールは旧ツールの登録済みハンドラをそのまま呼ぶ**薄いルーター**で、返り値の形は旧ツールのまま。他のキーは `target` / `op` / `view` ごとの旧引数(`dx12_tool_describe {name:"dx12_set_render_settings", target:"ssao"}` で引ける)。
  `values` を省略して旧引数をフラットに渡しても通る。`fix` は統合ツールの形(`dx12_edit_terrain {op, …}`)で返る。
- **alias 表(220 名)**: Core に同名で入る **19**(Core 18 + `dx12_batch`)/ 統合ツールが置換 **53**(描画設定 28 + perf 2 + capture 8 + terrain 10 + imgui 5)/ 長尾 **148**(`dx12_call` で使う: undo・アニメ・ナビ・Blender・アセット・git・decal・sequence・jev …)。
  置換された旧名は旧名のまま呼べ、`dx12_tool_describe` が `replacedBy`(統合ツールでの呼び方)を案内する。
- **命名規約**(lint = `toolSurface.test.ts`): `dx12_<動詞>_<対象>`。動詞が副作用を決める — `get/list/find/describe/check/validate/query` = read(`readOnlyHint:true`)/ `capture` = read + ファイル出力 /
  `set/create/add/remove/apply/edit/spawn/delete/open/save/write/attach` = write / `play/stop/run/record/step` = runtime / `git/eval/build/launch` = guarded。読み取り専用の一括許可は `mcp__dx12-engine__dx12_get_*` の 1 行で書ける。
- **サーバ `instructions`** は面ごとに変わる(core 面は Core の使い分けと `dx12_call_guarded` を載せる。2,048 字以内)。

**effect(副作用)の確定と見直し(M3)**: `read` / `write_scene`(Undo 可)/ `write_setting`(設定・エディタカメラ)/ `write_file`(Undo 不可)/ `runtime`(実行状態)/ `guarded`。

| 見直した点 | 内容 |
|---|---|
| `look_at` | `write_setting` → **`write_scene`**(rotation を書く。エンジン側の表も修正) |
| `screenshot_from` / `focus_and_screenshot` / `camera_path` | `write_scene` → **`write_setting`**(エディタカメラを動かして撮るだけ。Undo に積まれるシーン変更ではない) |
| `vfx_preview` / `sequence_preview` | `write_scene` → **`runtime`**(時間を進めて連写 / Play して流す) |
| `validate_layout` | 通常 read のまま。**`fix:'safe'|'all'` のときは書く**ので `dx12_call {dryRun:true}` は実行しない(`CONDITIONAL_WRITE`) |
| guarded の `destructiveHint` | `eval_lua` / `net_launch_test_client` / `git_checkout|merge|merge_abort|commit|push|pull` の 8 本に `destructiveHint:true` を足した(ヒントを足しただけ。許可・確認は緩めていない) |
| 据え置き | `select_entity` / `focus_camera` = write_setting、`reload_scripts` / `reload_assets` / `ui_click` = runtime、`navmesh_build|clear` = write_scene、`terrain_*` / `create_prefab` = write_file、`net_launch_test_client` = guarded、`git_fetch` = guarded(`readOnlyHint:true` の旧ヒントは変えない)、`diagnose` / `render_debug` / `perceive` = read |
| 旧ヒントの不整合(互換のため据え置き) | `benchmark` は `readOnlyHint:true` だが effect=runtime(時間が進む)/ `jev_ask` は `readOnlyHint:true` だが write_file(記録を書く)/ `git_fetch` は `readOnlyHint:true` だが guarded |

### 0-6. エンジンに method を足す最短手順(再起動不要で AI が使う)

1. `McpDefine("my_method", McpMeta{ .summary=…, .keywords=…, .category=…, .effect=McpEffect::WriteScene, .params={P(…)}, … }, DX12E_MCP_HANDLER { … });`(新規は meta を直接渡す。`source:"meta"` で必須/型/enum/範囲を**中央検証**する)
2. `pwsh tools\build.ps1` → エンジンを(再)起動する。
3. **何もしなくても** AI は `dx12_tool_search {query:"…"}` → `dx12_tool_describe {name:"my_method"}` → `dx12_call {name:"my_method", args:{…}}` で使える(MCP サーバ・Claude Code の再起動は不要。`ping.manifestHash` が変わるので次の呼び出しで取り込む)。
4. Core にも載せたければ **1 行**: `McpMeta` に `.expose = "core"`(末尾のフィールド)。MCP サーバが再接続時にマニフェストを取り直し、`dx12_my_method` を `tools/list` に足して `notifications/tools/list_changed` を送る
   (guarded な method は昇格しない)。TS ラッパを書きたいとき(専用の説明・合成処理)だけ `tools/mcp-server/toolset/*.ts` に `reg()` を足す(この場合はクライアントの再起動が要る)。
   静的に Core へ入れたい旧ツールは `coreSpec.ts` の `CORE_LEGACY` / `CORE_ORDER` / `CORE_DESCRIPTIONS` に足す。
5. 後始末(手作業): `docs/MCP.md` §4 の表 / `tools/mcp-server/manifest.snapshot.json`(`node scripts/gen_manifest_snapshot.mjs`。エンジン起動中に実行)/ ヒント(`searchHints.ts` と `eval/discovery_tasks.json`)。

**後始末の自動化の現状**: `npm run check:legacy-snapshot`(旧 220 の表面)・`schemaDrift.test.ts`・`toolSurface.test.ts` は自動。`docs/MCP.md` / README / `AGENTS.md` の表、`manifest.snapshot.json` の更新、Lua API の 4 点セット
(`McpLuaApi()` 辞書 + `docs/API_REFERENCE.md` + `docs/SCRIPTING.md` + `docs/index.html`)は**まだ手作業**。M4 で `npm run finalize`(`gen:docs` + `lua_api.json` → 辞書/表の生成と `--check` + manifest snapshot 更新 + `test:offline` を 1 コマンドに。
push は人の確認を挟む)にまとめる案。Lua API を足したときに 1 コマンドで何を更新するかの対応表は `dx12_guide {topic:"engine_dev"}` に書いてある。

---

### 0-7. 専用エンジン(フリート): エージェントごとに自分のエンジンを持つ

エンジンの TCP ブリッジは**単一クライアント**なので、複数のエージェント(Claude Code・Codex CLI の各セッション)が同じエンジンに繋ぐと奪い合いになる。
各セッションが**自分専用のエンジンを背景で起動**する方式にした(設計: `docs/MCP_FLEET_DESIGN.md`)。ポート・作業フォルダ・exe コピー・データ領域(`DX12E_DATA_DIR`)・使い捨てプロジェクトが全部別なので、
**ビルド(`tools\build.ps1`)が `build\release\DX12Engine.exe` を上書きしても `LNK1104` にならない**(実行中の exe は別の場所のコピー)。

| ツール | 内容 |
|---|---|
| `dx12_engine_launch {name?, project?, mode?, scene?, dpiScale?, args?, waitReadyMs?, confirm?}` | 専用エンジンを起動し、**このセッションの既定エンジンに束縛**する(以後の全ツールがそこへ向く)。`{engineId, port, pid, dir, exe, timing, ping}`。`project` 省略で使い捨てプロジェクトを自動生成 |
| `dx12_engine_list {discover?}` | 全セッションのエンジン一覧(自分のもの・他人のもの・孤児)と台数・上限・空き VRAM/RAM。`discover:true` で手動起動の候補ポートも探す(connect だけ) |
| `dx12_engine_stop {engine?, all?, force?, confirm?}` | 自分のエンジンを止める(プロセスツリーごと)。他人のエンジンは `force`+`confirm` が無ければ拒否(孤児は可)。`x-<port>`(attach)は外すだけ |
| `dx12_engine_attach {port?, engine?, readOnly?, confirm?}` | 手動起動/他人のエンジンを**読み取り専用**で見る(`effect:"read"` の method だけ通す。接続は最後の応答から 1.5 秒で閉じ、持ち主の接続枠を塞がない)。`readOnly:false` は `confirm:true` が要る |
| `dx12_engine_refresh {engine?, waitReadyMs?}` | ビルド後に exe コピーを最新へ差し替え、同じ id・ポート・プロジェクトで再起動する |
| `dx12_engine_use {engine}` | 既定エンジンの切替(`"none"` で従来の探索へ)。**core 面には出ない**(長尾: `dx12_call {name:"dx12_engine_use", args:{engine}}`) |

- **1 回だけ別のエンジンへ**: `dx12_call {name, args, engine:"<id|name|port>"}`。他の 220 ツールに `engine` 引数は足していない(共通の解決層 `EngineRouter` が向き先を決める)。
- **上限と資源**: 全セッション合計で最大 **3 台**、空き VRAM 2048 MB 未満・空き RAM 3072 MB 未満は起動を断る(`E_FLEET_LIMIT` / `E_FLEET_RESOURCE`。`cause` に数値、`fix` に止める候補)。
- **後始末(4 段)**: ① MCP サーバが 10 分操作の無いエンジンを止める ② MCP サーバ終了(stdio クローズ・SIGINT/SIGTERM・例外)で自分のエンジンを全部 `taskkill /T /F` ③ エンジン自身が `--owner-pid`(親が消えたら自殺)と `--idle-exit`(ping を除く操作が無い分数)で終了 ④ 孤児スイープ(launch / list / doctor / 30 秒ごと)。**殺すのはレジストリに載った pid だけで、イメージ名が記録と一致するときに限る**。
- **起動モード**: `background`(既定。窓は画面外・前面化しない)/ `headless`(窓なし)/ `visible`(**既定で拒否**。環境変数 `DX12_MCP_ALLOW_VISIBLE=1` と `confirm:true` の両方が要り、それでも実マウス・フォーカスを奪い得る)。
- **レジストリ**: `%LOCALAPPDATA%\UnoEngine\fleet\registry.json`(`registry.lock` の排他 + 一時ファイル → rename の原子的置換。古いロックは自動回収)。インスタンスは `instances\<id>\{bin,data}`、使い捨てプロジェクトは `projects\<id>\`(停止後も 24 時間残る)。
- **ポート**: 8860〜8899 を自動割当(8850〜8859 は手動用、8787 は従来の既定なので触らない)。
- **互換**: 束縛が無ければ従来の探索(`DX12_MCP_PORT` → ポートファイル → 8787)。旧 220 ツール・`dx12_call` の挙動・core/shell/full/legacy 面は不変(legacy 面にはフリートのツールを出さない)。
- **エンジンの起動引数**(フリートが付ける。手動起動でも使える): `--owner-pid <pid>`(その pid が消えたら自分で終了)/ `--idle-exit <分。小数可。0 で無効>`(`ping` と `describe_mcp_manifest` を除く MCP 操作・実入力が無い時間で自動終了)/ `--instance-id <id>`(`ping.instanceId`)。終了は `PostQuitMessage` 経由で、未保存の MCP 編集は先に保存する(保存確認のモーダルは出ない)。実装は `src/core/mcp/FleetGuard.h`(ctest `FleetGuardTests`)。
- `ping` の加算キー: `pid` `instanceId` `uptimeSec` `idleSec` `idleExitMin` `ownerPid` `vramUsedMB`(このプロセスの使用量。DXGI `QueryVideoMemoryInfo`)`vramBudgetMB`。

環境変数: `DX12_FLEET_DIR` / `DX12_FLEET_MAX`(3)/ `DX12_FLEET_IDLE_MIN`(10。小数可)/ `DX12_FLEET_MIN_FREE_VRAM_MB`(2048)/ `DX12_FLEET_MIN_FREE_RAM_MB`(3072)/ `DX12_FLEET_PORT_RANGE`(`8860-8899`)/ `DX12_FLEET_BUILD_DIR`(exe の元)/ `DX12_MCP_ALLOW_VISIBLE` / `DX12_FLEET_AUTOLAUNCH`(1 で、束縛も従来の接続先も無いとき最初の呼び出しで自動起動)/ `DX12_FLEET_DISABLE`(1 でフリートを止める)。

**クライアント設定の例**(推奨面は `core`: ツール検索が無いクライアントでも 40 本で足りる。Core に無い操作は `dx12_call`)

```toml
# Codex CLI: ~/.codex/config.toml
[mcp_servers.dx12-engine]
command = "node"
args = ["<REPO>/tools/mcp-server/index.ts"]
env = { DX12_MCP_SURFACE = "core" }
```

```bash
# Claude Code(全プロジェクト共通で登録)
claude mcp add dx12-engine -s user -e DX12_MCP_SURFACE=core -- node <REPO>/tools/mcp-server/index.ts
```

`DX12_MCP_PORT` は書かない(専用エンジンは束縛で向き先が決まる。書くと束縛が無いときの従来の探索先だけが固定される)。最初に `dx12_engine_launch` を撃つ。

### 0-8. ジョブ API(長い処理を裏で走らせ、id と進捗で扱う。M6)

Claude Code の「2 分を超える MCP 呼び出しの自動背景化」は**メイン会話だけ**で、サブエージェントや `claude -p` には効かない。長い処理(ビルド・テスト・撮影バッチ・cook)は自前の非同期 API にした(設計: `docs/MCP_FLEET_DESIGN.md` §10)。

| ツール | 内容 |
|---|---|
| `dx12_job_start {kind, args?, engine?, idempotencyKey?, timeoutSec?, waitSec?}`(**core**) | 即座に `{id, state, progress, hint, next}` を返す(10〜20 ms)。`waitSec` を付けるとその秒数まで終了を待つ |
| `dx12_job_status {id?, waitSec?, until?}`(**core**・read) | `{state, progress{phase, pct, message, etaSec, estimated?, sinceChangeSec}, queuePosition, elapsedSec, summary, error, artifacts, ownedByMe, orphaned?}`。`waitSec`(最大 300)= **long-poll**(終了まで、`until:"change"` なら次の変化まで)。`id` 省略は動いているジョブの一覧 |
| `dx12_job_cancel {id, force?}`(**core**) | process 型はプロセスツリーごと `taskkill /T /F`、待ち行列の中は開始せず cancelled、エンジン系は AbortSignal + エンジンの `cancel`。他セッションの生きたジョブは `force` が要る |
| `dx12_job_list {state?, kind?, mine?, limit?}`(長尾) | 履歴(MCP サーバ再起動前のジョブも出る) |
| `dx12_job_result {id}`(長尾) | 終了後の全文(summary・失敗したテスト・ビルドエラー・マニフェスト)。未完了は `E_JOB_NOT_FINISHED`。200 KB 超は要約 + ファイルのパス |
| `dx12_job_logs {id, tail?}`(長尾) | 出力の末尾(最大 500 行。実行中でも読める。20 MB を超えた分は記録しない) |

**状態**: `queued → running → succeeded | failed | cancelled | timeout`。**進捗の主経路はポーリング**(全クライアントで動く)。`progressToken` 付きの呼び出し(`dx12_job_status {waitSec}` / `dx12_job_start {waitSec}`)が待っている間だけ `notifications/progress`(`progress` は厳密に増加・`total:100`・`message` に `[kind] 進捗 (残り約 N 秒)`)も送る(ベストエフォート。**Claude Code / Codex が受け取るか・表示するかは未確認**)。

**種類(kind)**

| kind | executor | 内容・進捗 | 主な args |
|---|---|---|---|
| `build` | process | `tools\build.ps1`。**全セッションで直列**(同一サーバ内は `queuePosition`、他のセッションのビルド待ちは `phase:"waiting_lock"`)。進捗 = ninja の `[n/m]`(`compile` / `link` / `generate`)。エラーは `summary.errors[{file, line, code, message}]`(cp932 の MSVC 診断も文字化けしない)。ninja は工程が終わるまで行を出さないので、長い 1 工程の間は pct が止まる(`sinceChangeSec` が目安) | `target`(1 つ)/ `tests` / `jobs` / `dir` / `refreshEngines` |
| `ctest` | process | ヘッドレス単体テスト。`n/N`・ETA・失敗したテスト名・JUnit | `filter` / `exclude` / `jobs` / `testTimeoutSec` / `rerunFailed` |
| `ui_tests` | process | UI 自動テスト。**exe をインスタンス専用フォルダへコピーし、使い捨てデータ領域・背景起動・ポートは 8880〜8899 の空き**(元の exe は起動しない)。**既定で `build_game` を除外**(`--ui-tests-skip`。Game.exe が前面に出て人の操作を奪うため)。exe が `--ui-tests-skip` を持たなければ `E_UNSUPPORTED`(exe に UTF-16 のフラグ名があるかで判定)。途中経過が出ないので pct は経過時間からの見積もり(`estimated:true`)。終了後に JUnit(`summary.junit.failed`)。コピーは終了時に消す | `skip` / `project`(複製して使う。原本には触れない)/ `deep` / `speed` / `dpiScale` / `includeBuildGame` |
| `screenshot_batch` | inproc | カメラ × DPI 倍率 × バリアントを撮影 → 画像 + `manifest.json` + `contact_sheet.png`。`--dpi-scale` は起動引数なので、**いまのエンジンと違う倍率(や `launchArgs`)は専用エンジンを 1 台ずつ起動 → 撮影 → 停止**(束縛は変えない。起動直後は UI が崩れるので `step_frames 40` + `settleMs` 待つ)。バリアントの `calls` はトランザクションで巻き戻す | `cameras` / `dpiScales` / `variants` / `view`(final・imgui・scene)/ `deterministic` / `project` / `scene` / `contactSheet` |
| `bench` | inproc | `benchmark` を `runs` 回。fps・frameMs(avg / p95)・1% low の中央値と min/max。キャンセルでエンジンの `cancel {target:"benchmark"}` | `frames` / `runs` / `scene` / `camera` |
| `playtest` | inproc | 保存済み `.playtest` を 1 本ずつ再生(進捗 = 本数)。`dx12_run_playtests` と同じ判定 | `name` / `judge` |
| `vg_cook` | process | `vgeo_cook`。未ビルドなら `E_JOB_TOOL_MISSING` + `build {target:"vgeo_cook"}` | `input` か `genBench` / `output` / `threads` |
| `ue_import` | process | `tools/ue_cook`(ファイルを読むだけ)。`out` はリポジトリの外(PUBLIC のため) | `command` / `paks` / `usmap` / `package` / `out` |
| `external` | process | 任意の外部プロセス。**guarded**(`dx12_call_guarded` / `dx12_call {confirm:true}` 経由でだけ) | `command`(配列)/ `cwd` / `env` / `progress` |

**外部プロセスの進捗プロトコル**: 標準出力に 1 行ずつ `@progress {"pct":42,"phase":"cook","msg":"…","eta":30}` / `@progress {"done":3,"total":7}` / `@result {…}`(要約に載る)。`progress:"percent"` は「NN%」の行も拾う。壊れた JSON は無視。将来の `vg_cook` / `ue_cook` はこれを出せばそのまま進捗になる。

**永続化と復元**: `%LOCALAPPDATA%\UnoEngine\jobs\<id>\`(`DX12_JOBS_DIR`)に `state.json`(書き手は manager)・`spec.json`・`live.json`(書き手は runner。5 秒の心拍つき)・`log.txt`・`result.json`・`artifacts\`。**process 型は切り離した runner プロセス(`jobs/runner.ts`)が子を走らせる**ので、MCP サーバを再起動しても走り続け、再起動後の `dx12_job_status` で進捗・結果が引ける(持ち主が消えていれば `orphaned:true`。孤児は別セッションから cancel できる)。inproc 型(エンジンを呼ぶもの)はサーバが終わると `E_JOB_INTERRUPTED` になる。終わったジョブは 7 日・200 件で GC。runner の生死は pid(安価)で見て、心拍が 45 秒止まったときだけイメージ名(node)を確認する(pid の使い回し対策)。

**同時実行**: 総数 3(`DX12_JOBS_MAX_RUNNING`)・build 1・ctest 1・ui_tests 1・external 2・エンジン系は 1 エンジンにつき 1 本。**キャンセル・タイムアウトは、自分が記録した runner の pid(イメージ名が node のときだけ)のプロセスツリーを落とす**(無関係なプロセスは触らない)。エンジンを使うジョブの間は、そのエンジンのフリートのアイドル自動終了を防ぐ。

**冪等キー**: `idempotencyKey` は 24 時間、同じ kind・args の再送に前回のジョブを返す(`idempotentReplay:true`)。別の kind / args なら `E_IDEMPOTENCY_CONFLICT`。

**doctor 統合**: `dx12_doctor` に `jobs`(件数・上限・動いているジョブ・直近の終了)と `JOBS_ACTIVE` / `JOBS_ORPHANED` / `JOBS_RECENT_FAILED`。`dx12_guide {topic:"jobs"}` に手順。

環境変数: `DX12_JOBS_DIR` / `DX12_REPO_DIR`(build.ps1 の場所。配布リポジトリには無い)/ `DX12_JOBS_MAX_RUNNING` / `DX12_JOBS_KEEP_DAYS` / `DX12_JOBS_KEEP_MAX` / `DX12_JOBS_POLL_MS` / `DX12_JOBS_KILL_ON_EXIT=1`(サーバ終了時に process 型も止める)/ `DX12_JOBS_FALLBACK_ENCODING`(既定 shift_jis。UTF-8 として不正な行の復号)/ `DX12_JOBS_DISABLE=1` / テスト用 `DX12_JOBS_BUILD_CMD`・`DX12_JOBS_CTEST_CMD`(JSON 配列)。エラー: `E_JOB_NOT_FOUND` / `E_JOB_NOT_FINISHED` / `E_JOB_NOT_OWNER` / `E_JOB_TOOL_MISSING` / `E_JOB_FAILED` / `E_JOB_TIMEOUT` / `E_JOB_INTERRUPTED` / `E_JOB_RUNNER_LOST` / `E_JOB_DISABLED`(§8)。

### 0-9. エディタ操作(コマンド表・状態・通知・選択。M7)

「窓を開く」「元に戻す」「エンティティを作る」のようなエディタ操作を、**座標を探してクリックする前に、名前で実行する**口。エディタの**コマンド表**(`src/editor/EditorCommandTable.h` の `kCommands`・`ToolWindows.h` のツール窓 = `window.*`・`EditorCreateTable.h` の作成 = `create.*`。メニュー・ショートカット・コマンドパレットが使う**唯一の源**)からエンジンが生成する。**TS 側にコマンドを 1 件も書かない**(表にコマンドが増えれば自動で出る)。実行はメニューやキーと**同じ経路**(`cmd::Execute`)。梯子の順は「① `dx12_editor_command` → ② 専用ツール(`dx12_set_component` など)→ ③ `dx12_imgui`(仮想入力。値欄・ツリー・スライダなどのウィジェットだけ)」。

| ツール | 内容 |
|---|---|
| `dx12_editor_command {op, id?, args?, query?, category?, kind?, enabledOnly?, guardedOnly?, detail?, dryRun?, idempotency_key?}`(**core**) | `op:"list"` = コマンド一覧(`id` / 表示名 / カテゴリ / `kind`(command・window・create)/ キー / **`enabled` と `disabledReason`(いま実行できるか・理由)** / `guarded` / `osDialog` / `opensModal` / `blockedNow` / `hasArgs`)。`query` は id・日本語ラベル・英名・キー表記の曖昧検索。`detail:true` で英名・説明・効果・引数・例まで。`op:"describe"` = 1 件の詳細(引数と例)。`op:"run"` = 実行。結果の `effects` に開閉した窓・トースト・Play / 一時停止の変化・選択・エンティティ数・未保存・Undo・**開いたモーダル**・作成したエンティティ(`created`)。`dryRun:true` は実行せず影響(対象・件数・戻せるか・いま実行できるか)を返す(guarded でも通る) |
| `dx12_editor_state {scope?, limit?}`(**core**・read) | エディタの今の状態(下の「state の仕様」)。`scope` = all(既定)/ selection / windows / layout / modal / mode / undo / toasts / perf |
| `dx12_editor_modal {action?}`(長尾) | `action` = get(既定。`editor_state` の modal と同じ)/ dismiss(いちばん上の**安全に閉じられる**モーダルをキャンセルと同じに閉じる)。安全なのは new_scene / save_as / new_script / new_shader / shortcuts / about のみ。結果 `{dismissed:true, id, title, modal}`。閉じられないもの(unsaved_confirm / autosave_recovery / matgraph_* / コマンドパレット)は `E_UNSUPPORTED`、閉じるものが無いときは `E_NOT_FOUND`。M8 で `respond`(ボタン名指定)を足す予定 |
| `dx12_editor_notify {message, level?, seconds?}`(長尾) | 人のエディタ画面の右下にトースト通知(`info` / `success` / `warn` / `error`)。AI から人へ「生成完了」「要確認」を静かに伝える(OS 通知は使わない)。`--background` の窓は画面外なので**人の画面には出ない**(`dx12_editor_state {scope:"toasts"}` で出たことは読める) |
| `dx12_editor_select {mode?, entities?, names?, query?, tag?, guids?, limit?, focus?}`(長尾) | 選択の変更。`mode` = set(既定)/ add / remove / toggle / clear。名前(完全一致)・id・名前の部分一致(`Wall*`)・タグ・guid、複数選択、`focus:true` でカメラを寄せる。選択は `edit.*` コマンド(複製・グループ化・フォーカス・削除)の対象になる。旧 `dx12_select_entity`(1 体だけ)は無傷で残る |

エンジン method(`describe_mcp_manifest` に載る。`dx12_call` でそのまま撃てる): `editor_command_list`(read)・`editor_command_run`(write_setting・遅延応答・dryRun preview)・**`editor_command_run_guarded`(guarded)**・`editor_state`(read)・`editor_notify`(write_setting)・`editor_select`(write_setting)・`editor_modal`(write_setting。get / dismiss)。

**コマンドごとの `args`**(`describe` で引ける): `window.*` = `{state:"open"|"close"|"toggle"}`(**既定 open**。表の Execute はトグルだが、AI が 2 回撃って閉じてしまわないよう、既に目的の状態なら `changed:false` で何もしない)/ トグル系(`view.fill` `view.flyMode` `view.toggle2D` `view.viewportBar` `view.outline` `layout.bottomMaximize`)= `{state:"on"|"off"|"toggle"}` / `create.*` = `{position:[x,y,z], name}`(既定はカメラ前・床との交点。`create.ui*` は位置を無視。`create.terrain` / `create.sculpt` は専用窓が開くだけ)/ それ以外は引数なし。

**guarded な(確認が要る)コマンド**: `edit.delete`(選択を丸ごと削除。何が消えるか見えない)・`file.save` / `file.saveAs`(シーンファイルを上書き)・`file.closeProject`(ランチャーへ戻る)・`file.open`(OS のファイルダイアログ)。**接頭辞 `build.` / `git.` / `shell.` のコマンドが将来増えれば自動で guarded**。`opensModal` = アプリ内のダイアログ / モーダルを開くもの(`file.new` `file.saveAs` `file.newScript` `file.newShader` `palette.commands` `palette.quickOpen` `layout.save` `layout.slots`)。`osDialog` = OS のダイアログを開く経路があるもの(`file.open` のみ)。分類はエンジン側の分類表で、新コマンドは何もしなければ normal(明示した id だけ guarded)。
- `editor_command_run` は guarded なコマンドを **`E_GUARDED` で断る**(`fix` に承認つきの実行口)。実行は `editor_command_run_guarded`: **core 面は `dx12_call_guarded {name:"editor_command_run_guarded", args:{id}}`(毎回ユーザー承認)、full / shell 面は `dx12_call {name:"editor_command_run_guarded", args:{id}, confirm:true}`**。エンジン側の確認トークンは M5 の仕組みで付く(§13)。先に `dryRun:true` で影響を確認する。
- **OS ダイアログを開くコマンド(`file.open`)は、仮想入力 / 背景モードでは承認しても実行拒否**(`E_UNSUPPORTED` + `details.reason:"os-dialog"`、`fix` は `dx12_open_scene {path}`)。人の画面にダイアログを出さないため。
- **モーダル / ダイアログ / コマンドパレットが開いている間は、コマンドは `E_MODAL_OPEN`**(キー操作が効かないのと同じ。`details.modals` に種類)。`dx12_editor_state {scope:"modal"}` で確認し、**`dx12_editor_modal {action:"dismiss"}` で閉じて撃ち直す(★ImGui のモーダルは Esc では閉じない。実測)**。閉じられないもの(未保存の確認・自動保存の復旧・マテリアルグラフのダイアログ)は `E_UNSUPPORTED`(`details.id`)で、`dx12_imgui {op:"find"}` + `pointer` でボタンを押す。コマンドパレット(`kind:"palette"`)だけは自前で Esc を処理するので `dx12_imgui {op:"key", key:"Esc"}`。`file.new` などモーダルを開くコマンドの結果は `effects.modalsOpened` と `next`(state → `dx12_editor_modal`)で案内する。エンジンの `E_MODAL_OPEN` の `fix` も `editor_modal {action:"dismiss"}` と `editor_state {scope:"modal"}`。
- いま実行できない(Play 中の Editor 専用コマンド・選択なし・Undo 履歴なし・AI のトランザクションが開いている)は `E_MODE_CONFLICT` + `details.reason`。未知の id は `E_NOT_FOUND_COMMAND` + `didYouMean`(近い id)+ 撃ち直し。引数違いは `E_INVALID_PARAM` + `details.args`。

**state の仕様**(`dx12_editor_state`): `{scope, frame, ...}` + 選ばれたセクション。

| セクション | 中身 |
|---|---|
| `selection` | `{count, primary:{entityId,name,guid?}\|null, entities[](最大 50), truncated?, hovered?}` |
| `windows` | `{open[], openCount, tools[{id,title,open,slot,menu,category,imguiName?,visible?,docked?,focused?,collapsed?,rect?}], focusedWindow, hoveredWindow}`(ツール窓は `ToolWindows.h` の表から) |
| `layout` | `{workspace, savedLayouts[], bottomDockMaximized, dockRatios{left,right,bottom,rightSplit}, display{width,height,dpiScale}, dockNodes[{id,parent,axis,tabs[],central?,size}], structureHash}` |
| `modal` | `{blocking, count, modals[{kind:"imgui-modal"\|"editor-dialog"\|"palette"\|"popup", id, title, source, canDismiss, dismissHint}], popupOpen, note}`。editor-dialog の `id` = new_scene / save_as / new_script / new_shader / unsaved_confirm / autosave_recovery。`canDismiss:true` = `editor_modal dismiss` で閉じられる(palette は Esc)/ `false` = ボタンで選ぶ。**blocking のとき TS 側が `advice` と `next`(`dx12_editor_modal dismiss` / ボタンなら `find` / palette なら Esc)を足す** |
| `mode` | `{engineMode, playing, paused, headless, background{mode,toolWindow}, virtualInput, dpiScale, scene{path,dirty,generation,entityCount}, aiTransaction{open,label?}, viewport{camera{position,forward}, viewMode, view2D, flyMode, gizmo{mode,space}, fill, workspace}}` |
| `undo` | `{canUndo, canRedo, undoDepth, redoDepth, nextUndo?, nextRedo?, nextUndoIsAi?, recentUndo[], recentRedo[], dirty, editSeq, savedSeq}` |
| `toasts` | `{live, recent[{seq,kind,text,count,ageSec}](新しい順。`limit` で件数)}` |
| `perf` | `{fps, frameMs, cpuMs?, drawCalls?, entityCount}` |

**安全**: 実マウス・実キーボード・OS のカーソル・前面化には一切触れない(コマンド表の実行関数を呼ぶだけ)。`dx12_editor_command` を「座標を探してクリックする」代わりに使うのが最短で最も安全。禁止事項(computer-use / 前面化 / `SendInput` など)は `dx12_guide {topic:"editor"}` に固定文で載っている。

**Core の入れ替え(M7)**: `dx12_editor_command` / `dx12_editor_state` を Core に入れるため、`dx12_play_script` と `dx12_engine_list` を長尾へ移した(頻度が低く、`dx12_call` で名前・引数そのまま代替できる。オフラインの選択率: Core 面のみ recall@3 97.5%(M6 は 97.6%)・総合 97.8%(不変)。`eval/editor_tasks.json` = エディタ操作 9 タスクは全ツール検索 96.3% / Core 面のみ 100%。検索語は eval を見ながら調整した数字)。

### 0-10. 宣言的シーン生成(`dx12_apply_scene_spec`。M11)

「何を置きたいか」を **SceneSpec(JSON)** で 1 回渡す。**差分だけを 1 トランザクションで作り、自動検証して、失敗は AI が撃ち直せる形(specPatch)で返す**。1 体ずつ `create_entity` を並べる代わりの口(実エンジン実測: 200 体 + 床の適用が約 0.4 秒。`create_entity` + `set_transform` を 1 体ずつ 200 回は約 7.6 秒 = **約 20 倍**)。書き方・例 5 本・よくある失敗は `dx12_guide {topic:"scene_spec"}`(`guides/scene_spec.md`)。

| ツール | 内容 |
|---|---|
| `dx12_apply_scene_spec {spec \| specRef+patch, mode?, verify?, prune?, detail?, async?}`(**core**) | `mode:"plan"` = 差分計画だけ(何も書かない)/ `"apply"`(既定)= 適用 + 検証。`dryRun:true`(`dx12_call` の native dryRun)は plan と同じ。`prune:true` は削除なので **guarded**(core 面 `dx12_call_guarded`、full 面 `dx12_call {confirm:true}`。plan は承認なしで撃てる)。`async:true` は大規模な仕様をジョブ(`dx12_job_start {kind:"scene_spec"}`)で実行 |
| `dx12_scene_spec_export {owned?, name?, only?, prefix?}`(長尾・read) | 現在のシーン → SceneSpec。往復(シーン → 仕様 → 適用)で同じシーンになる(ダイジェスト一致をテスト) |

**SceneSpec v1**(`sceneSpec/types.ts`): `{version:1, name?, entities:[…], lighting?, look?, sun?, scene?, navmesh?, verify?}`。エンティティの主なキー: `name`(差分のキー)・`id`(省略で name。**name を変えても rename_entity で同じ実体に追従**)・`kind`(box / sphere / plane / model / prefab / empty / camera / light(`light`)/ trigger / particle_emitter / decal / ui_* / **fps_player**(本体 + カメラ))・`group`・`parent`・`at`(m。`null` の軸は place が決める)・`rotation`(度・YXZ)・`scale` か `size`(実寸 m。併用不可)・`color`(`"#rrggbb"` は見たままの色になるよう sRGB→リニア変換。`[r,g,b]` はエンジンの値そのまま)・`material`・`texture`・**`collider:"static"|"dynamic"`**(rigidBody + 形に合うコライダーの略記)・`components`・`script`・`tags`・`data`・`place`・`pattern`・`lookAt`。単位はメートル、モデルは読み込み時に実寸 m(`scale` は倍率。M4 の読み込みサイズ規約どおり)。**指定した項目だけを管理する**(書かなかった rotation / scale / 軸は既存の物では手で変えた値を尊重)。

**相対配置の規則**(`sceneSpec/expand.ts`。決定論): 軸はワールド right=+X / left=-X / front=+Z / back=-Z / above=+Y / below=-Y。AABB は解析(box ±0.5・sphere 半径 0.5・plane 50m 四方 × scale)+ モデルは `asset_info` の実寸 + 仕様の外の物は `get_bounds` の実測。回転・スケール・親子は `get_bounds` と同じ式(ローカル AABB の 8 頂点を変換。**実エンジンの `get_bounds` と 2mm 以内で一致することを実機で確認**)。`place:{relativeTo,side,gap,align}`(面と面のすき間。他の軸は中心揃え・y は底揃え)/ `{on}`(上に載せる)/ `{ground}`(足元を地面へ)/ `{snap:true}`(置いた後にエンジンの `snap_to_ground`)/ `offset` / `lookAt`(yaw = atan2(dx,dz)、pitch = -atan2(dy, 水平距離)。実エンジンの `look_at` と一致)。`at` の非 null の軸が優先。親があれば解はワールドで出してから親のローカルへ戻す。**パターン**: `grid` / `ring` / `line` / `along`(壁に沿って等間隔)/ `scatter`(seed 付き・`minSpacing`・`exclude`。**位置と向き/大きさは別の乱数列**なので、同じ seed の 2 つのパターンは必ず同じ位置に並ぶ = 木の幹と葉を重ねられる)。名前は `<name>_<NN>`。

**処理の流れ**(`sceneSpec/index.ts`): ① `validateSpec`(スキーマ + 意味検査。error があればエンジンに 1 つも書かない)→ ② 展開 + 相対配置の解決 → ③ 現在のシーンを読む(`list_entities` + `get_hierarchy` + `get_entity`)→ ④ **差分計画**(`{create, update, replace, delete, unchanged}` と各行の理由・変更前後・影響・`cost{engineCalls, waves, estimatedMs}`。所有者の印 = `data.__spec`(仕様名)/ `__id` / `__o`(読み戻せない部分)。**prune はこの印が自分の仕様名で、いまの仕様に無い物だけ**を消す)→ ⑤ **1 トランザクションで適用**(波ごとに並列: グループ根 → 削除 → 改名 → 生成 → 親子 → Transform → 色・部品・タグ・data・スクリプト → snap → スクリプト値。エンジンは 1 フレームで溜まった要求を全部処理するので往復が消える)→ ⑥ ナビメッシュ(要求時)→ ⑦ **自動検証**(`verify`: layout = `validate_layout`(この仕様が作った物の error はロールバック)/ naming = 命名規約(既定 warn)/ reachable = `dx12_check_reachable` / scene = `validate_scene`)→ ⑧ commit(失敗はロールバック)→ ⑨ 設定(lighting / look / sun / scene。エンジンの rollback で戻らない設定なので**検証が通った後**に撃つ。仕様の太陽の明示値が設定より優先)。

**失敗の返し方(自己修正)**: 検証・配置・適用・自動検証のどの段階の失敗も `E_VALIDATION_FAILED`(上限超過は `E_OUT_OF_RANGE`)+ `issues[{path(JSON Pointer), code, severity, message, cause, didYouMean, validValues, specPatch, entity}]` + `details{stage, specRef, specPatch(全部まとめた RFC 6902)}` + **`fix[0] = {tool:"dx12_apply_scene_spec", args:{specRef, patch}}`(そのまま撃ち直せる。仕様の全文は再送しない)**。`specRef` はサーバが直近 16 件の仕様を持つ。同じエンティティに複数の issue が付いたら、優先順位(DUPLICATE > 接地 > コライダー > 重なり > ちらつき)で直し方を 1 つにする。エンジンは、検証エラーでは書かず、適用中・適用後の失敗ではロールバック済み(部分適用を残さない)。code: `E_UNKNOWN_PARAM` `E_MISSING_PARAM` `E_BAD_TYPE` `E_BAD_ENUM` `E_OUT_OF_RANGE` `E_NOT_FOUND_ENTITY` `E_NOT_FOUND_ASSET` `E_NOT_FOUND_COMPONENT` / `E_SPEC_DUPLICATE_NAME` `E_SPEC_CYCLE` `E_SPEC_CONFLICT` `E_SPEC_LIMIT`(5,000 体)`E_SPEC_BOUNDS_UNKNOWN` `E_SPEC_KIND_CHANGE` `E_SPEC_RENAME_CONFLICT` / `E_LAYOUT_*` `E_NAMING_*` `E_UNREACHABLE` / warn `W_UNIT_SUSPECT`(cm と m の取り違えの疑い)`W_SCATTER_SHORT` `W_ASSETS_UNCHECKED`。

**エンジン側の変更(最小)**: `set_component {component:"tags", data:["a","b"]}` を受け付けるようにした(以前は data がオブジェクト必須で、`describe_components` の記述どおりの文字列配列を渡すと必ず断られていた。空配列で全部外す)。ほかは既存 method(`create_entity` / `spawn_model` / `set_transform` / `set_parent` / `set_component` / `snap_to_ground` / `rename_entity` / `transaction_*` / `validate_layout` / `navmesh_build` ほか)だけで動く。

**Core の入れ替え(M11)**: `dx12_apply_scene_spec` を Core に入れるため `dx12_scene_write` を長尾(`dx12_call`)へ移した(M3 で「`apply_scene_spec` が来るまでの代役」として入れた本人。名前・引数・返り値はそのまま使える。「シーン JSON を直接書く」の検索では引き続き上位に出る)。Core 面は 40 本のまま。

**実測(実エンジン専用インスタンス `m11`・使い捨てプロジェクト)**: 冪等(再適用は created 0 / updated 0・シーンのダイジェスト不変)・往復(ダイジェスト一致)・ロールバック(壊れたモデルの `spawn_model` が同じ波の途中で失敗 → 変更ゼロ)・prune(承認・Undo 1 回で戻る)・自己修正(壊した仕様 16 件で 93.8%)は `scripts/sceneSpecReal.ts`。例 5 本のコンタクトシートは `scripts/sceneSpecShots.ts`。

## ★ 最重要: 遅延同期の仕組み(旧 `queued:true` は廃止)

`create_entity` / `spawn_model` / `spawn_prefab` / `duplicate_entity` / `delete_entity` /
`open_scene` / `new_scene` / `play` / `stop` は **「遅延同期」** で動く。

- エンジン内部ではフレーム境界(GPU cmdList が使える瞬間)で実処理する。
- **ただし Node 側は id で待つだけでよい。** エンジンは処理完了後に【同じ id】で本物の result を返す。
- 旧仕様の `{queued:true}` は**もう返ってこない**。`entityId` / `sceneGeneration` 等の本物の値が直接返る。
- **「名前で list して探す」旧パターンは完全廃止。** 返ってきた `entityId` を使い続ける。

```
AI → dx12_create_entity(type:"box", name:"Floor")
        ↓ (フレーム境界まで待機)
エンジン → {entityId: 42, name: "Floor", sceneGeneration: 7}
AI → dx12_set_transform(entity: 42, ...)   ← そのまま entityId を使う
```

---

## 1. セットアップ

MCP サーバは **別リポジトリ [ryuto-alt/dx12-mcp](https://github.com/ryuto-alt/dx12-mcp) で配布**する
(エンジン配布物には同梱されない)。エディタの「MCP / AI Bridge」窓がインストールコマンドを表示する。

### 配布エンジン利用者(推奨)

```powershell
git clone https://github.com/ryuto-alt/dx12-mcp "$env:USERPROFILE\dx12-mcp"
cd "$env:USERPROFILE\dx12-mcp"
./install.ps1        # Linux/macOS: ./install.sh
```

`%USERPROFILE%\dx12-mcp` に入れておくと、エディタの「MCP / AI Bridge」窓が自動検出して
登録コマンドをワンクリックコピーできる。

### エンジンをソースから開発している場合

このリポジトリの `tools/mcp-server` がソース・オブ・トゥルース(dx12-mcp リポジトリへは
`tools/mcp-server/publish.ps1` で同期する)。そのまま使える:

```bash
cd tools/mcp-server

# Windows:
./install.ps1

# Linux / macOS:
./install.sh
```

スクリプトは Node v24+ を確認 → `npm install` + 自己テスト(`npm test`、エンジン不要) →
**Claude Code と Codex の両方へ自動登録**（`claude mcp add --scope user` / `codex mcp add`）まで行う。
手で貼るコマンドは無く、CLI が入っていないクライアントの分だけ手順を表示する。
再実行しても壊れない（remove → add で冪等）。登録後はクライアントを再起動すること。

> `--scope user` を使う。Claude Code の既定 `local` スコープは**そのディレクトリでしか有効でない**ため、
> 別のプロジェクトで作業すると「ツールが出ない」になる。

手動の場合:

```bash
cd tools/mcp-server
npm install
node test.ts        # 自己テスト(フレーミング/相関/エラー)
```

Node v24+ が `.ts` を直接実行するため `tsc` ビルドは不要。

---

## 2. 接続（install スクリプトが失敗したとき / 手で入れたいとき）

通常は `install.ps1` / `install.sh` が両クライアントへ自動登録するので、この節は不要。
`<REPO>` は clone した絶対パスに置換する(Windows でもパスは `/` 区切りで可)。

### Claude Code(CLI)
```bash
claude mcp add dx12-engine --scope user -- node <REPO>/tools/mcp-server/index.ts
```

### Claude Code(`.mcp.json`)
`tools/mcp-server/.mcp.json.example` をコピーして `.mcp.json` を作り `<REPO>` を置換する。
`.mcp.json` は `.gitignore` 済み=各自で生成すること。

> 注意: 既定では `env` に `DX12_MCP_PORT` を書かないこと。書くとポート自動探索(`%TEMP%/dx12_mcp.port`)が
> 無効化され、エディタが 8787 を取れず 8788 等に回った時に繋がらなくなる。ポートを固定したい時だけ書く。

```json
{
  "mcpServers": {
    "dx12-engine": {
      "command": "node",
      "args": ["<REPO>/tools/mcp-server/index.ts"]
    }
  }
}
```

### Codex(`~/.codex/config.toml`)
```toml
[mcp_servers.dx12-engine]
command = "node"
args = ["<REPO>/tools/mcp-server/index.ts"]
```

---

## 3. ポート自動探索

エンジンは起動時に空きポートを **8787〜8797** の範囲で自動採番し、確定したポート番号を
`%TEMP%\dx12_mcp.port`(Windows)または `$TMPDIR/dx12_mcp.port` に書き込む。

Node(engineClient)はポートを以下の順で解決する:

| 優先度 | 手段 |
|--------|------|
| 1 | 環境変数 `DX12_MCP_PORT` |
| 2 | ファイル `<os.tmpdir()>/dx12_mcp.port` |
| 3 | 既定値 `8787` |

ホストは `DX12_MCP_HOST`(既定: `127.0.0.1`)で変更可。別マシンのエディタを叩く場合は
SSH ポートフォワード推奨(エンジン側は `127.0.0.1` のみ待受)。

---

## 4. ツール一覧（全 220 ツール + shell 5 本）

> shell 5 本(`dx12_tool_search` / `dx12_tool_describe` / `dx12_call` / `dx12_doctor` / `dx12_guide`)は §0。以下の 220 本は従来どおり(名前・引数・成功時の返り値は不変)。

MCP ツール名は `dx12_` 接頭辞付き。同期欄: **同期** = 即返り、**遅延同期** = フレーム境界後に本物の値が返る。

### 4-1. 読み取り系(全て同期)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_ping` | `{}` | `{pong, mode, entityCount, sceneGeneration, currentScene, assetsDir, scriptsDir, baseDir, projectShaderDir, cwd, virtualInput, background, dpiScale, protocolVersion:4, manifestHash…, pid, instanceId, uptimeSec, idleSec, idleExitMin, ownerPid, vramUsedMB, vramBudgetMB}` ※末尾 8 個は専用エンジン(フリート。§0-7)用の加算キー。`idleSec` は ping と `describe_mcp_manifest` を除く操作・実入力からの秒、`vramUsedMB` はこのプロセスの使用量(DXGI)、取れなければ -1 ※**`assetsDir` はエンジンが返す正**（`protocolVersion 4` から）。ログの絶対パスから推定する必要はもう無い |
| `dx12_describe_mcp_params` | `{method?:string}` | `{methods:{<method名>:[{key,type}]}, count, globalKeys:["idempotency_key"], note}` ※**エンジンのディスパッチ表そのもの**。`type` は `bool`/`int`/`number`/`string`/`vec3`/`object`/`any`。`"親.子"` は入れ子オブジェクトのキー（例 `skybox.envMapPath`）。TS スキーマとのドリフト検出はこれを正にすること |
| `dx12_list_entities` | `{verbose?:bool, name_prefix?:string, component_type?:string}` | `{entities:[{entityId,id,name,componentTypes?}], count, sceneGeneration}` |
| `dx12_get_entity` | `{entity:int}` | `{entityId, componentTypes:[...], sceneGeneration, ...(全コンポーネント値)...}` |
| `dx12_find_entity` | `{name:string}` | `{entityId, name}` または `null` |
| `dx12_query_entities` | `{tag?:string, box?:[minX,minZ,maxX,maxZ]}` | `{entities:[{entityId,name}], count}` ※tag か box のどちらか必須 |
| `dx12_list_scenes` | `{}` | `[{path, name}]` |
| `dx12_list_assets` | `{type?:"model"\|"texture"\|"script"\|"audio"\|"scene"\|"prefab"\|"shader"}` | `[{path, type, name}]` |
| `dx12_get_mode` | `{}` | `{mode:"Editor"\|"Playing"}` |
| `dx12_get_log` | `{lines?:int=50}` | `["ログ行", ...]`(末尾N行) |
| `dx12_describe_components` | `{component?:string}` | `{components:[{jsonKey, settable, removable, fields:[{name,type,default}], note?}]}` |
| `dx12_get_scene_settings` | `{}` | `{skybox:{envMapPath, iblIntensity, skyboxIntensity, drawSkybox}, decalAtlasPath, atmosphere:{全項目}, atmosphereState:{太陽の高度/方位・IBL 再ベイク回数・GPU 時間}, note}` ※物理大気は [docs/ATMOSPHERE.md](ATMOSPHERE.md) |
| `dx12_get_post_process` | `{}` | ポストプロセス全フィールド(約25エフェクトの `<name>On`/パラメータ) |
| `dx12_get_ssao` | `{}` | `{enabled, radius, bias, intensity, power, sampleCount, blur}` |
| `dx12_get_contact_shadow` | `{}` | `{enabled, rayLength, thickness, bias, intensity, steps, maxDistance, fadeDistance}` |
| `dx12_get_taa` | `{}` | `{enabled, sampleCount, feedbackMin, feedbackMax, varianceGamma, jitterScale, debugVelocity, active, fxaaSuppressed}` |
| `dx12_get_render_scale` | `{}` | `{scale, renderResolution:{width,height}, displayResolution:{width,height}, pending, note}` ※**内部解像度スケール**（#16 レンダー解像度と表示解像度の分離） |
| `dx12_get_depth_prepass` | `{}` | `{enabled, note}` ※深度プリパスの単独強制（計画10 A2 の A/B スイッチ） |
| `dx12_get_ssr` | `{}` | `{enabled, intensity, maxDistance, thickness, maxSteps, stride, roughnessCutoff, edgeFade, bias}` |
| `dx12_get_ssgi` | `{}` | `{enabled, intensity, radius, thickness, rayCount, stepCount, clampValue, feedback, iblFallback}` |
| `dx12_get_volumetric_fog` | `{}` | `{enabled, density, albedo, anisotropy, heightFalloff, heightRef, distance, depthDistribution, ambient, sunIntensity, lightScattering, temporal, temporalBlend, extendBeyondRange, debugMode, active}` |
| `dx12_describe_lua_api` | `{}` | binding ごと(entity/transform/Vec3/self/scene/input/camera/physics/audio/nav/ui/fx/events/globals/prelude)の**静的辞書**。★MCP で見えるコンポーネントと Lua から読める API は違う（entity から直接読めるのは transform だけ）。Lua を書く前にこれで確認する |
| `dx12_get_lua_component_state` | `{entity?/name?}` | `{scriptPath, enabled, started, loadError, errorMessage, properties:[{name,type,value,isOverride}]}` ※未上書きの既定値も含む（`get_entity` は保存済みの上書きしか出さない）。`loadError=true` なら `errorMessage` に Lua の traceback がそのまま入る |
| `dx12_get_script_errors` | `{}` | `{count, mode, errors:[{entityId,name,scriptPath,message}]}` ※★`dx12_play` の結果に `scriptErrors>0` が出たら次はこれ。どのエンティティが壊れたか分からない状態で使う |
| `dx12_get_play_session` | `{maxEvents?:int=400, maxSamples?:int=200, goal?:[x,y,z], goalName?:string, judge?:bool=true}` | `{started, recording, durationSec, frames, fpsMin, summary:{...}, events:[{t,kind,detail}], samples:[{t,fps,camPos,...}], judge?:{source, confusion:{value(0..4), level, troubled}, cause:{id, label, hint, confidence}, words, uncertain[], findings:[], cost}}` ※**`dx12_play` を押した時点で自動的に記録が始まる**（開始ツールは無い）。人間に遊んでもらってから取りに来る用。`detail` のキー名は `dx12_key_press` にそのまま渡せる。★`judge` はプレイの判断段(§4-16): 区間ごとの事実を言葉にして Brief と一緒に Jev へ聞き、Brief が狙っていない迷い・苛立ちの度合い(`troubled` は実測の線 1.09 以上)と主な原因を返す。判断は間引かない全体で数える |
| `dx12_read_lua_component` | `{path:string}` | `{path, code}` ※既存 .lua のソースをそのまま読む |
| `dx12_read_shader` | `{path:string(assets/shaders相対)}` | `{path, code, compiled}` ※既存カスタムシェーダーのソースをそのまま読む(compiled は直近の既知のコンパイル成否) |
| `dx12_list_shader_templates` | `{}` | `[{name, title, summary}]` ※同梱のシェーダー雛形(water / ocean / particle_ember 等)を列挙する。★白紙から 200 行の HLSL を書くのは失敗率が高い。まずこれで動くものを起こしてから削る方が確実 |
| `dx12_describe_shader_contract` | `{kind?:"mesh"\|"particle"\|"sprite"\|"screen"(既定 mesh)}` | カスタムシェーダーが使える定数(b0/b1)・テクスチャ・入出力・注意点を kind ごとに返す ※★cbuffer はオフセットで対応が決まるので、1 つでもズレるとコンパイルは通るのに値だけ化ける(エラーが出ないので気付けない)。書き始める前に必ず読むこと |
| `dx12_raycast` | `{origin:[x,y,z], direction:[x,y,z], maxDistance?:f}` | `{hit, distance?, point?, normal?, entityId?, name?, instanceIndex?, character?}` ※Playing 中のみ意味のある結果。CharacterController のキャラもカプセルとして当たる（`character:true`。始点がそのカプセルの中なら無視） |
| `dx12_overlap_box` | `{center:[x,y,z], halfExtents:[x,y,z], maxResults?:int}` | `{entities:[{entityId,name}], count}` ※Playing 中のみ。判定は相手の実形状（回した壁・メッシュの凹みも見る。キャラも含む） |
| `dx12_overlap_sphere` | `{center:[x,y,z], radius:f, maxResults?:int}` | `{entities:[{entityId,name}], count}` ※Playing 中のみ。形状判定（上と同じ） |
| `dx12_get_physics_state` | `{entity?:int, name?:string}` | `{entityId, hasRigidBody, velocity:[x,y,z], hasCharacterController, isGrounded, world}` ※Playing 中のみ。entity / name を省くと物理世界の診断値 `{world:{bodies, activeBodies, maxBodies, maxBodyPairs, maxContactConstraints, updateErrorFlags, updateErrorSteps, failedBodies, droppedSteps, lastFrameSteps}}` だけを返す（`updateErrorFlags`: 1=接触キャッシュ 2=ボディ対 4=接触拘束の上限超過。超えた分の接触は無視される＝物が床を抜ける原因） |
| `dx12_audio_state`（エンジン method `audio_state`） | `{}` | `{device{ready,status,channels,sampleRate}, listener{position}, limits{maxVoices,logicalCapacity,realVoices,virtualVoices,voiceChurnCount,voiceChurnMs}, buses:[{name,parent,volume,muted,lowpassHz,snapshotGain,snapshotLowpassHz,effectiveGain,chainGain,reverbSend,reverbReturn,voiceLimit,realVoices,virtualVoices,peakDb,rmsDb,peakNowDb,builtin}], voices:[{id,clip,bus,priority,volume,fade,pitch,audibility,gainDb,spatial,distance,distanceGain,occlusion,reverbSend,loop,virtual,virtualReason,bgm,paused,stream,positionSec,lengthSec,bufferedChunks?,underruns?,loopPointsSec?}], bgm{path,playing}, snapshot{current,target,progress,duration,defined}, reverb{available,defaultPreset,defaultWet,dominantZone,targetWet,currentWet,zones:[{name,preset,weight,wet,priority}],params{roomMb,roomHfMb,decayTime,decayHfRatio,reflectionsMb,reflectionsDelay,reverbMb,reverbDelay,diffusion,density}}, streams{count,memoryBytes,underruns,decodeMs,chunksDecoded,watchdogPumps,audioSecondsDecoded,decodeMsPerAudioSecond}}` ※**AI が音を観測する口**（即時・Editor/Play どちらでも）。数値だけを返す。dB は dBFS で **-120 = 無音**（-inf は JSON に載らないため）。メーターはバスの出力（フェーダー後）で、ピークは 0.5 秒保持→24dB/s 下降、RMS は 0.3 秒平滑（`peakNowDb` は直近 10ms）。`virtualReason`: `inaudible`（遠い / バスがミュートで -60dB 未満）/ `limit`（上限で待機）/ `stolen`（より大事な音に押し出された）/ `noDevice`（音声デバイス無し）。 |
| `dx12_brain_state`（エンジン method `brain_state`） | `{entity?, name?}` | 省略で `{brains:[{entityId,name,enabled,running,action}], mode}`、指定で 1 体の `{entityId, name, time, tick, action, decision{chosen, actions:[…]}, history, switches, blackboard, perception, movement, errorCount}`(decision.actions の final = weight × 考慮事項の積 + 粘り) ※ゲーム AI(Brain)の観測。読み取り専用。Brain は Play 中だけ動くので実行時の状態は Play 中にしか無い |
| `dx12_validate_scene` | `{path?:string}` | `{pass, exitCode, report, scenePath}` ※`--validate` をヘッドレス子プロセスで実行。省略時は現在のシーン |
| `dx12_get_anim_state` | `{entity:int}` | `{hasSkeletalAnimation, clips:[クリップ名...], boneCount, currentClip, clipTime, speed, looping, blending, hasController, graphPath, graphLoaded, layers:[{name,weight,state,normalizedTime,transitioning,transitionTo,transitionProgress,masked}], parameters:{名前:値}, footIK:{enabled,weight,resolved,bones,boneNames,leftContact,rightContact,leftLift,rightLift,pelvisOffset,leftNormal,rightNormal}}` ※`dx12_play_anim` の clipName 選びと、接地の破綻をスクショ無しで検知するのに使う |
| `dx12_describe_anim_graph` | `{entity:int}` または `{path:string}` | `{source, graph:{version,parameters,clipEvents,extraClips,layers:[{name,weight,blend,mask,defaultState,states,transitions}]}}` ※`.animfsm` の構造を返す。ステート名/パラメータ名の確認に。**TS 側未定義**（B12 と同様） |
| `dx12_net_status` | `{}` | `{available, role:"Offline"\|"Host"\|"Client", isConnected, localClientId, tick, syncedEntityCount, players:[{id,rttMs,bytesSent,bytesReceived}], config:{tickRate,snapshotRate,maxPlayers,defaultPort}, testRole, testJoinAddress}` |
| `dx12_screenshot` | `{path?:string, deterministic?:bool=false, settleFrames?:int=8(1..240), gizmos?:bool=true}` | PNG 画像ブロック + text(`{path(絶対パス), width, height, source:"sceneRT(pre-post)", note}`) ※**ポストプロセス前の `m_sceneRT`**。グレーディング / ブルーム / ゴッドレイ / ビネット / LUT / FXAA / デバンド / **TAA の解決結果が一切写らない**。見た目を判断するなら `dx12_screenshot_final` を使うこと。★`gizmos:false` はこの経路では `deterministic:true` のときだけ効く（既定は直前フレームの読み戻しで撮り直さないため） |
| `dx12_screenshot_final` | `{path?:string, deterministic?:bool=false, settleFrames?:int=8(1..240), gizmos?:bool=true, format?:"png"\|"pfm"\|"exr"="png", formats?:string[], width?:int, height?:int}` | **遅延同期**。`{path, files, linear, offscreen, width, height, source:"backbuffer"\|"offscreen", postApplied, deterministic, gizmos, taa, mode, note}` ※**Q2（校正）で追加した引数は全部省略可で、省略すると従来と 1 ビットも変わらない**。`format:"pfm"`/`"exr"` = **ポスト前の線形 float**（トーンマップ前・露出前のシーン参照 Rec.709 RGB。TAA / DoF / モーションブラー後・ブルーム前。パストレーサー `render_reference` と同じ PFM/EXR writer なので `tools/parity` がそのまま読める。物理ライティング単位ならシーン値は nit）。`formats:["png","pfm"]` で同じフレームから複数形式。`path` の拡張子は無視され形式ごとに同じ基準名（`a.png` → `a.pfm`）。pfm/exr のときは画像ブロックではなく JSON（`files` に形式ごとのパス、`linear` に規約）で返る。`width`+`height` = **任意解像度のオフスクリーン出力**（エディタ / ウィンドウ / 16:9 レターボックスに依存せず、シーン系 RT をその大きさで作って描き出し、撮影後に元へ戻る。1 辺 16〜8192・総画素 8192x4096 まで・pfm/exr は 4096x4096 まで。GPU メモリの見積が空きの 60% を超えたら撮影前にエラー。エディタのアイコン・ゲーム内 UI 画像・画面全体のカスタムシェーダーは写らない。`deterministic:true` と併用でき、同じ設定ならビット一致）。起動引数 `--size WxH` が `width`/`height` の既定 ※**バックバッファ（＝ポスト適用後の最終画）のビューポート矩形**。ImGui を描く前にコピーするので**エディタのパネル / ギズモは写らない**＝ゲームと同じ絵。サイズはウィンドウ全体ではなく**シーンビューの矩形**。★`gizmos:false` で**この 1 枚だけ**エディタのデバッグ描画（カメラの視錐台の水色の線 / 選択枠 / ライト・カメラのアイコン / 物理・ナビのワイヤ / 床グリッド）を止めて撮る。選択を外しても消えない「アクティブなカメラの視錐台」もこれで消える。**戻す呼び出しは不要 ── 撮影状態と一緒に破棄されるので次の 1 枚では必ず元どおり**（`dx12_render_debug` と同じ作法） |
| `dx12_screenshot_game_view` | `{}` | PNG 画像ブロック ※**アクティブな `CameraComponent`（ゲームカメラ）視点**で 1 フレーム描いて返す。Editor 中でも Play せずに画角・構図を確認できる。アクティブなカメラが無いとエラー |
| `dx12_ui_screenshot` | `{}` | PNG 画像ブロック ※エディタウィンドウ全体(ImGuiパネル込み)。ゲーム内UI/UIエディタの見た目確認用(scene RT には UI が写らない)。★仮想入力モード(§4-18)中は PrintWindow ではなく dx12_imgui_screenshot と同じバックバッファ読み戻しになる(最小化/画面外/背面でも撮れる。遅延同期) |
| `dx12_render_debug` | `{mode:string, frames?:int=3(1..120), gain?:number=1, depthRange?:number=100, exposure?:number=1}` | `{path(絶対パス), mode, width, height, toneMapped:bool, warnings:[string], mode_engine:"Editor"\|"Playing"}` ※**中間バッファの可視化**（「なぜ変に見えるか」の切り分け用）。`frames` フレーム描いてからスクショを撮り、**必ず元の設定へ戻す**。必要な機能（TAA/SSAO/コンタクトシャドウ/SSR/SSGI）は一時的に自動で ON にし、その旨を `warnings` に返す。返り値の `path` を画像として読むこと |
| `dx12_ui_tree` | `{}` | `{canvases:[{entityId, name, uiCanvas:{refWidth,refHeight,...}, children:[{entityId, name, components, uiRect, resolvedRect:[x,y,w,h](キャンバス空間px), text?, children}]}]}` ※UIレイアウトの数値確認 |
| `dx12_ui_click` | `{name?/entity?, x?:0..1, y?:0..1, move?:bool}` | `{target, viewportLocal:{x,y}, viewport:{width,height}, moveOnly, stepped, mode, note}` ※**ゲーム内 UI を合成ポインタで押す**。テストプレイで AI がメニューを操作する口。★実マウスとまったく同じ経路（レイキャスト→最前面判定→押下キャプチャ→release-inside で確定）へ流すので、**前面に別の要素が被っているボタンは押せない**のが正しく再現される（名前で onClick を直接呼ぶ方式ではない）。name/entity でその要素の中心、x,y（ビューポート基準 0..1）で任意の座標。**何も無い所を押してフォーカスを外す**のにも使える。押す→離す→配送の 3 フレームを回してから返る(遅延同期)。Lua の onClick が走るのは Play 中だけ |
| `dx12_ui_design_brief` | `{genre:"cinematic"\|"tactical"\|"fantasy"\|"horror"\|"arcade"\|"cozy", screen:"title"\|"hud"\|"inventory"\|"settings"\|"result"\|"dialog"\|"other", tone?}` | 画面固有の構図・階層・制約・アンチパターン。UI生成前に呼ぶ |
| `dx12_ui_audit` | `{strictness?:"balanced"\|"strict", screen?:"title"\|"hud"\|"inventory"\|"settings"\|"result"\|"dialog"\|"other", judge?:bool=true}` | 現在のUIを数値監査。`{pass,score,grade,summary,issues[],metrics, judge?:{source, briefFit, findings:[{code, intended, keep}], uncertain[], passExcludingKept, scoreExcludingKept, gradeExcludingKept, notAsked[], cost}}`。崩れ/重なり/可読性/入力遮断/過装飾を検出。★`judge` は判断段(§4-16): 好みのルール 7 種だけを Brief と照らして keep するか聞く(押せない・読めない系は聞かない = notAsked) |
| `dx12_ui_compose` | `{blueprint:{theme,prefix,root}}` | dock/stack/grid と意味的roleからUI一式を制約付き生成。失敗時ロールバック。生成後はaudit→screenshot必須 |
| `dx12_get_editor_camera` | `{targetDistance?:number=10}` | `{position, forward, target, targetDistance, yawDeg, pitchDeg, fovYDeg, aspect, nearZ, farZ, orthographic, overridden, mode}` ※シーンビューを描いてるカメラの状態。**`target` は `position + forward * targetDistance`**。そのまま `dx12_set_editor_camera {position, target}` へ渡すと同じ yaw/pitch に戻る＝読み返し検証ができる |
| `dx12_get_bounds` | `{entity:int, includeChildren?:bool, perSubmesh?:bool=false}` | `{min, max, center, size, hasMesh}` ※ワールド空間 AABB(回転/親子変換込み)。配置座標の計算に。★`perSubmesh:true` で `{submeshes:[{index, name, materialName, triangles, localMin/localMax/localSize, worldMin/worldMax/worldCenter/worldSize}], submeshCount, largestSubmesh}` も返る＝**「モデルの一部だけ変な位置に飛んでいる。どの部品か」が 1 回で分かる**。`index` は `dx12_pick` の `submeshIndex` と同じ並び。★glTF/FBX の JSON 内の並びとは一致しない（ローダがノード単位に展開するため）ので照合は `name` / `materialName` で |
| `dx12_get_hierarchy` | `{}` | `{roots:[{entityId, name, children:[...]}], count, sceneGeneration}` ※シーンの親子ツリー |
| `dx12_asset_info` | `{path}` | モデル: `{meshCount, totalVertices, totalFaces, materialCount, boneCount, hasSkeleton, animations:[{name,durationSec}], aabbMin/Max(ノード変換込みのワールド AABB)}`、テクスチャ: `{width, height, mipLevels, format, isCubemap}`、他: `{type, fileSizeBytes}` |
| `dx12_view_texture` | `{path, maxSize?:int=1024}` | PNG 画像ブロック ※dds/tga/hdr も変換して見られる。キューブマップは先頭面のみ |
| `dx12_pick` | `{x?,y?(px) \| u?,v?(0..1), all?:bool, maxHits?:int=16, includeIcons?:bool=true, trianglePrecise?:bool=true, maxCandidates?:int=64}` | `{hits:[{entityId,name,submeshIndex,distance,worldPos,worldNormal,isIcon}], count, totalHits, truncated, screen, viewport, mode}` ※**エディタの左クリック選択と同じ `RaycastScene`**。座標系は `dx12_screenshot` / `dx12_project_world_to_screen` と同じ |
| `dx12_raycast_precise` | `{origin:[x,y,z], direction:[x,y,z], maxDistance?:f=1000, all?:bool, maxHits?:int=16, trianglePrecise?:bool=true, maxCandidates?:int=256}` | `dx12_pick` と同形式 + `{origin, direction, maxDistance}` ※**描画メッシュの三角形基準**。`dx12_raycast`(物理コライダー基準・Playing 限定)とは別物 |
| `dx12_project_world_to_screen` | `{entity?/name?}` | `{x, y, visible, depth, w, width, height, mode}` ※エンティティのワールド座標を、今シーンビューを描いているカメラで画面ピクセルへ投影する(`dx12_screenshot` と同じカメラ。Playing 中はアクティブなゲームカメラ)。★`w<=0` はカメラ背面 |
| `dx12_terrain_sample` | `{entity?/name?, points?:[[x,z]...] (最大512)}` | `{entityId, name, origin, resolution, worldSize, cellSize, boundsXZ, minHeight, maxHeight, samples:[{x,z,height,worldY,normal,slopeDeg,inside}], count}` |
| `dx12_list_lights` | `{limit?:int=50, cursor?:int}` | `{lights:[{entityId,name,type,position,slot,color,intensity,range?,direction?,innerConeDeg?,outerConeDeg?,castShadows?,overBudget,effective}], count, total, cursor, nextCursor, has_more, budget:{total,perCluster,point,spot,directional,shadowSpot,shadowPoint}, warnings:[...]}` ※**上限超過は無言で描画されない**ので必ずここで確認する。クラスタードライティング(Forward+)で点/スポットの個別上限は撤廃され、**合計 1024 灯 / 1 クラスタ 128 灯**が上限。**影は spot 4 / point 2 のまま** |
| `dx12_diagnose` | `{only?:string[], fast?:bool}` | `DeepDiag::RunAll` の JSON(`{version, engine, checks:[{id,title,checked,errors,warnings,infos,issues,omitted,skipped}], summary:{checks,errors,warnings,infos,ok,unknownIds}, checkIds, note}`) ※`summary.errors > 0` だけが失敗 |

### 4-2. 編集系(同期)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_set_transform` | `{entity:int, position?:[x,y,z], rotation?:[x,y,z](Euler度), quaternion?:[x,y,z,w], scale?:[x,y,z]}` | `ok` |
| `dx12_set_component` | `{entity:int, component:string(jsonKey), data:object\|array}` | `{entityId, component}` |
| `dx12_remove_component` | `{entity:int, component:string}` | `{entityId, removed}` |
| `dx12_set_parent` | `{entity:int, parent?:int}` | `ok` ※parent 省略で親解除 |
| `dx12_group_entities` | `{entities?:int[], names?:string[], name?:string}` | `{groupId, name, count}` ※空の親にまとめる（原点・単位スケール＝見た目は不変）。入れ子の子は自動除外、Undo 可 |
| `dx12_rename_entity` | `{entity:int, name:string}` | `{name}` ※重複は連番付与 |
| `dx12_select_entity` | `{entity:int}` | `{selected}` | ※1 体だけ選ぶ旧ツール(無傷で残る)。**複数選択・名前パターン・追加/解除・全解除・フォーカスは `dx12_editor_select`(§0-9)** |
| `dx12_focus_camera` | `{entity:int}` | `{cameraPos:[x,y,z], target, distance}` |
| `dx12_set_pbr` | `{entity:int, metallic?:f, roughness?:f, uvScaleU?:f, uvScaleV?:f, alphaMode?:"auto"|"opaque"|"mask"|"blend", alphaCutoff?:f, opacity?:f, emissiveIntensity?:f, emissiveColor?:[r,g,b]}` | `{entityId, metallic, roughness, uvScaleU, uvScaleV, alphaMode, alphaCutoff, opacity, emissiveIntensity, emissiveColor}` ※**自己発光**は `emissiveIntensity`（0..64、0 で消灯、負でマテリアルに従う）。色を省くと白。ライティングも影も通さず最終色へ加算するので 1 を超えるとブルームが乗る（看板 2..5 / 天井照明 4..10 / 非常口サイン 3..6 が目安）。発光の形をテクスチャで指定するなら `dx12_set_texture` の `slot:"emissive"` と併用。 ※**透明**は `alphaMode`。既定の `auto` はモデル側（glTF の `alphaMode`）に従う。`mask` は `baseColor.a < alphaCutoff` を discard（葉・フェンス・角膜。**影も同じ形に抜ける**）、`blend` は半透明（不透明の後にカメラから遠い順で描く。深度を書かず影も落とさない）。`opacity` は 1 未満なら `alphaMode` を省いても半透明になる（ガラス・水面）。詳しくは docs/AUTHORING.md §7.5 |
| `dx12_set_mesh_shader` | `{entity:int, shaderPath?:string(assets/shaders相対), alphaBlend?:bool}` | `{entityId, shaderPath, alphaBlend, skinnedFallbackWarning}` ※shaderPath省略/空文字で既定Forwardに戻す。alphaBlend省略時は既存値を維持、既定false(不透明固定でPSのalpha出力は無視される)。true でSrcAlpha/InvSrcAlphaブレンド(DepthWrite OFF) |
| `dx12_set_mesh_shader_params` | `{entity?/name?, effect?:f, params?:f[](最大4), paramsB?:f[](最大3)}` | 適用後の現在値一式 ※カスタムシェーダーの自由枠(b0 の `effectValue`/`shaderParams`/`shaderParamsB`)へ値を書く。★これが無いとシェーダーは【貼れるが動かない】(割当直後は全パラメータ 0)。各値の意味はシェーダー自身のヘッダコメント(`dx12_read_shader` で読める)。ルート定数なので毎フレーム撃っても安い。時間で動かす(徐々に溶ける等)なら Trigger の AnimShaderParam を使うこと(Lua からの口はまだ無い) |
| `dx12_set_sprite_shader` | `{entity:int, shaderPath?:string(assets/shaders相対), alphaBlend?:bool}` | `{entityId, shaderPath, alphaBlend, worldSpaceWarning}` ※Sprite2D専用・world-spaceのみ対応。MeshRendererのシェーダーとは頂点/ルートシグネチャの契約が異なる(docs/AUTHORING.md §6.1)。shaderPath省略/空文字で既定Spriteシェーダーに戻す |
| `dx12_set_scene_settings` | `{skybox?:{envMapPath?, iblIntensity?, skyboxIntensity?, drawSkybox?}, atmosphere?:{preset?:earth\|mars\|haze\|twilight, enabled?, timeOfDay?, timeSpeed?, latitudeDeg?, dayOfYear?, sunMode?, driveSun?, driveIBL?, sunIlluminance?, aerialPerspective?, …}}` | `{applied, envMapRebake, atmosphere?}` ※`atmosphere` は物理大気(既定 OFF=従来の空)。指定分だけ適用・Undo 1 エントリ。[docs/ATMOSPHERE.md](ATMOSPHERE.md) |
| `dx12_set_post_process` | 約25エフェクトの `<name>On`/パラメータ(指定分のみ適用) | `{applied}` |
| `dx12_set_post_process`（Q2 校正: 露出 / トーンマップ / ライティング単位） | `{lightingUnits?:0\|1, exposureMode?:0\|1\|2, ev100?, evComp?, aeMinEv100?, aeMaxEv100?, aeSpeedUp?, aeSpeedDown?, aeLowPercent?, aeHighPercent?, tonemapper?:0..5, filmSlope?, filmToe?, filmShoulder?, filmBlackClip?, filmWhiteClip?}` | `{applied}` ※既定値では従来と絵が 1 ビットも変わらない。`lightingUnits:1` = 物理単位（太陽 lux・点/スポット cd の逆二乗・空/IBL/自己発光 nit。シーン RT の 1.0 = 1 nit）。`exposureMode:1` = 手動 EV100（係数 `1/(1.2·2^(ev100−evComp))`。既定 EV100=15 が「露出 0」の基準。マスター `enabled` が OFF でも効く）/ `2` = 自動（ヒストグラム。平均輝度を 18% グレー×2^evComp へ。`aeMin/MaxEv100` が上下限）。`tonemapper` = 0 ACES / 1 AgX / 2 なし(ガンマのみ) / **3 UE Filmic / 4 線形(クリップのみ) / 5 Khronos PBR Neutral**（3〜5 は sRGB OETF）。詳細・根拠・不確実性は `docs/PARITY_HARNESS.md` の Q2 節 |
| `dx12_set_post_process`（被写界深度） | `{dofOn?, dofFocusDist?, dofFocusName?, dofAperture?, dofFocalLength?, dofBlurSize?, dofFocusRange?}` | `{applied}` ※**DoF は絞り基準（物理モード）が既定**。`dofAperture`＝F 値（既定 2.8）と `dofFocalLength`（mm。**0 でカメラの画角から導出**）から薄レンズの錯乱円 `CoC = |z-zf|/z * f²/(N*(zf-f))` を出し、35mm 判のセンサ高 24mm で px 化する。＝**画面解像度に依存する `dofBlurSize` は「暴走防止の上限」へ後退**し、絵作りは F 値だけで決まる。`dofAperture:0` で旧来の `dofFocusRange` 方式（範囲基準）へ戻る。`dofFocusName` にエンティティ名を入れると**毎フレームそのエンティティまでのビュー距離を合焦距離に使う**（＝被写体に合焦したまま寄る/回るカメラワークが Lua 無しで書ける。空なら `dofFocusDist`）|
| `dx12_set_ssao` | `{enabled?, radius?, bias?, intensity?, power?, sampleCount?, blur?}` | `{applied}` |
| `dx12_set_contact_shadow` | `{enabled?, rayLength?, thickness?, bias?, intensity?, steps?, maxDistance?, fadeDistance?}` | `{applied}` ※太陽(平行光)専用のスクリーン空間近接遮蔽。正射/2Dビューでは自動無効 |
| `dx12_get_normal_filter` | `{}` | `{enabled, strength, varianceClamp, geometricBlend}` |
| `dx12_set_normal_filter` | `{enabled?, strength?, varianceClamp?, geometricBlend?}` | `{applied}` ※**法線マップフィルタリング**。1 画素の中に法線マップの山谷が何個も入る状況（高いタイリング / 遠景 / 面を舐める角度）で ①鏡面がちらつく ②**光源が面を舐める角度で N·L の符号が裏返り、面が黒い斑点で埋まる** の 2 つが起きる。スクリーン空間の法線微分から画素内の法線分散を見積もり、分散を GGX のラフネス(α) へ足し込み（Toksvig / Kaplanyan 相当）、分散が大きい画素は法線を幾何法線へ寄せる（＝平均法線の復元）。**既定 ON**（strength=2 / varianceClamp=0.25 / geometricBlend=2）。`enabled:false` または `strength:0` で完全に従来どおり。シーン JSON の `normalFilter` に保存される |
| `dx12_set_taa` | `{enabled?, sampleCount?, feedbackMin?, feedbackMax?, varianceGamma?, jitterScale?, debugVelocity?}` | `{applied}` ※テンポラルAA。ONの間は `fxaaOn` が無視される。深度+速度プリパスが常時走る。正射/2Dビューでは自動無効 |
| `dx12_set_render_scale` | `{scale:0.25..1.0}` | `{...get と同じ...}` ※**内部解像度スケール**。3D シーン系の RT（sceneRT / 深度 / SSAO / コンタクトシャドウ / TAA 履歴・速度 / G-Buffer / SSR・SSGI / ブルーム / DoF / ゴッドレイ / 歪み）だけを `scale` 倍で確保し、最終(uber)パスで表示解像度へ引き伸ばす。**UI / ImGui / エディタのアイコンとギズモは常に表示解像度のまま**＝文字はボケない。`settings.json` の `"render_scale"` に保存される（起動時とプロジェクト切替時に読む）。★変更は**次のフレーム先頭**で反映される（内部で `WaitIdle` するのでフレーム外でしか作り直せない）。反映後は TAA / SSR / SSGI / ボリュメトリックフォグの**時間履歴が全部捨てられる**（座標系が変わるため。持ち越すとゴーストする）。★`dx12_screenshot` は**レンダー解像度**、`dx12_screenshot_final` は**表示解像度**で返る。`dx12_pick` / `dx12_project_world_to_screen` の座標系もレンダー解像度 |
| `dx12_set_depth_prepass` | `{enabled:bool}` | `{enabled, note}` ※**深度プリパスを単独で ON にする**。通常は SSAO / コンタクトシャドウ / TAA / SSR / SSGI / DXR のどれかが要求したときだけ走るが、これを立てると単独で走る（正射 / 2D ビューでは自動的に無効）。`dx12_perf_stats` / `dx12_benchmark` の `gpuPassMs.depthPrepass` が**プリパスの描画だけ**の実測値で、`prepassSsao` はそれを含む「プリパス一式（SSAO / コンタクトシャドウ / SSR・SSGI 生成込み）」。**そのシーンでオーバードローがどれだけあるか＝オクルージョンカリングの余地**を測るための道具。`settings.json` の `"render_depth_prepass"` に保存される |
| `dx12_get_shadow_pcss` | `{}` | `{enabled, lightTanAngle, maxPenumbraTexels, blockerSearchTexels, temporalDither, active, temporalDitherActive, applied:false, note}` ※`active` は「ON でも実際に走る条件（影 ON かつ透視カメラ）」を満たしているか |
| `dx12_set_shadow_pcss` | `{enabled?:bool, lightTanAngle?:0.001..0.5, maxPenumbraTexels?:1..64, blockerSearchTexels?:1..64, temporalDither?:bool}` | `{...get と同じ..., applied:true}` ※**PCSS（ソフトシャドウ）**。CSM の固定幅 PCF を「ブロッカー探索 → 可変ペナンブラ」へ置き換え、接地部は鋭く・離れるほど柔らかい影にする。**OFF で従来の 3x3 PCF に戻る（絵はビット一致）**。`lightTanAngle` は太陽の角半径の tan（実際の太陽は 0.0044＝ほぼ硬い影。既定 0.05 は誇張値）。`temporalDither` は **TAA 有効時のみ**効く（無効時に回してもチラつくだけなのでエンジンが自動で切る）。シーン JSON の `shadowPcss` に保存される |
| `dx12_get_dxr` | `{}` | `{supported, raytracingTier:"1.2" or "none", highestShaderModel:"6.8", shadowEnabled, shadowSunAngle, shadowNormalBias, shadowMaxDistance, shadowIntensity, aoEnabled, aoRadius, aoRayCount, aoIntensity, aoPower, aoCombineWithSsao, maxInstances, forceBuildTlas, shadowActive, tlasReady, stats:{instances, blasCount, blasBytes, blasTriangles, tlasBytes, scratchBytes, instanceDescBytes, skippedSkinned, skippedTransparent, droppedOverLimit, tlasReuseFrames, bytesPerTriangle}, applied:false, note}` ※`shadowActive` は「ON でも実際に走ったか」。`stats` は直近フレームの加速構造の実測値 |
| `dx12_set_dxr` | `{shadowEnabled?:bool, shadowSunAngle?:0..20(度), shadowNormalBias?:0..1(m), shadowMaxDistance?:0..100000(m, 0=無限), shadowIntensity?:0..1, aoEnabled?:bool, aoRadius?:0.01..100(m), aoRayCount?:1..8, aoIntensity?:0..1, aoPower?:0.01..8, aoCombineWithSsao?:bool, maxInstances?:0..32768, forceBuildTlas?:bool, ddgiFollowCamera?:bool, ddgiSpacing1?:0.1..100(m), ddgiBudgetMs?:0.05..20}` | `{...get と同じ..., applied:true}` ※**DXR 1.1 inline raytracing（RayQuery）**。RT サン影は既存のコンタクトシャドウ枠(t11)、RT-AO は既存の SSAO 枠(t8) へ書くので**ルートシグネチャは 1 DWORD も増えない**。**非対応 GPU では `error_code` を返す**（要 DXR Tier 1.1 かつ SM 6.5）。★スキンドと半透明は加速構造に入らないので従来どおり CSM が担当し、フォワードの `min()` で合成される（RT 影が有効なフレームは CSM が「RT の担当ぶん」を描かなくなる＝排他）。シーン JSON の `raytracing` に保存される |
| `dx12_render_reference` | `{spp?:1..1048576(既定 256), bounces?:1..64(既定 8), size?:[w,h](既定 [1920,1080]), camera?:{position:[x,y,z], target:[x,y,z], fovDeg?, lensRadius?, focusDist?}, output?:string(拡張子なし), seed?:int, maxRadiance?:number(0=クランプ無し), frameBudgetMs?:0.5..200(既定 12), maxSeconds?:number, formats?:["pfm"|"exr"|"png"], exposure?, lightFalloff?:"engine"|"physical", sunAngularRadiusDeg?, russianRoulette?, forceLambert?, normalMaps?, quantizeLikeForward?, background?, tileSize?, samplesPerDispatch?, note?, waitSec?:0..3600}` | `{accepted:true, jobId, state:"requested", size, spp, bounces, seed, frameBudgetMs, output, hint}`（`waitSec` 指定時は完了まで待って最終状態を返す） | ※**DXR パストレーサー（地上真値レンダラ。パリティ基盤 Q1a）**。inline RayQuery の compute（専用ルートシグネチャ・専用 TLAS のスナップショット）で、NEE（太陽 / 点・スポット / エミッシブ面 / 環境）+ MIS + 多重バウンス + ロシアンルーレット。**BSDF はフォワードと同じ PBR**（GGX + Lambert）なので、フォワード + DDGI/SSGI/SSR との差は光輸送のアルゴリズム差だけ。出力は線形 HDR（`.pfm` / `.exr`）+ 簡易プレビュー `.png` + メタ `.json`。**即座に返り**、1 フレームに使う GPU 時間は `frameBudgetMs` まで（エンジンを固めない）。同じシード + 同じ設定 = 同じ結果（決定論）。**既定 OFF**（要求が来るまで何も確保せず、通常の描画経路には触れない）。制約: 仮想ジオメトリはプロキシ（低ポリ）でトレース / スプラット地形・カスタムシェーダは標準 PBR で近似 / スキンドは法線マップ無し。詳細は `docs/PATH_TRACER.md` |
| `dx12_render_reference_status` | `{preview?:bool, waitSec?:0..3600}` | `{state:"idle"|"requested"|"preparing"|"running"|"finalizing"|"done"|"failed"|"cancelled", jobId, busy, progress:{phase,pct,message,etaSec}, samples:{done,target}, gpu:{msPerUnit,msPerSpp}, scene:{instances,materials,lights,triangles,emissiveTriangles,skippedSkinned,vgProxyInstances,customShaderInstances,splatTerrainInstances,snapshotMs}, output:{base,files[],truncated,sppDone,nanSamples,renderSeconds}, preview?, error?}` | ※`progress` は M6 のジョブ API の Progress と同じ形。`preview:true` は今までの累積を `<output>.preview.png` へ書く（running のときだけ。GPU の完了待ちで数十 ms 止まる） |
| `dx12_render_reference_cancel` | `{save?:bool}` | `{cancelled, state, hint}` | ※実行中のリファレンスレンダーを止める。`save:true` でそこまでの累積（spp は目標未満）を保存する（既定は捨てる） |
| `dx12_set_ssr` | `{enabled?, intensity?, maxDistance?, thickness?, maxSteps?, stride?, roughnessCutoff?, edgeFade?, bias?}` | `{applied}` ※スクリーン空間反射。深度プリパスのG-Bufferと前フレームカラーをレイマーチして IBL の鏡面を置き換える。反射は1フレーム遅れる。ONの間は深度+速度プリパスが常時走る。正射/2Dビューでは自動無効 |
| `dx12_set_ssgi` | `{enabled?, intensity?, radius?, thickness?, rayCount?, stepCount?, clampValue?, feedback?, iblFallback?}` | `{applied}` ※スクリーン空間GI。前フレームカラーを間接光源にして IBL の拡散(irradiance)を置き換える。`iblFallback` を切るとカメラ回転で明るさが変動する。正射/2Dビューでは自動無効 |
| `dx12_set_volumetric_fog` | `{enabled?, density?, albedo?, anisotropy?, heightFalloff?, heightRef?, distance?, depthDistribution?, ambient?, sunIntensity?, lightScattering?, temporal?, temporalBlend?, extendBeyondRange?, debugMode?}` | `{applied}` ※froxel ボリュメトリックフォグ。視錐台に沿った 3D テクスチャ(160x90x64)へ散乱を焼いて合成する＝光の筋が立体的に見える。有効にすると VRAM を 28MB 確保する。GodRays と同時に有効にすると太陽の散乱が二重計上される。正射/2Dビューでは自動無効 |
| `dx12_get_occlusion` / `dx12_set_occlusion` | `{enabled:bool}` | `{enabled, active, ready, pyramid{width,height,mips}}` ※**Hi-Z オクルージョンカリング**。深度プリパスの深度から階層 Z ピラミッド（max 縮約）を作り、壁の裏に完全に隠れた描画を GPU 側で落とす。判定結果は D3D12 の**プレディケーション**へ直接渡すので読み戻しゼロ・遅延ゼロ（前フレームの結果を使う方式で起きる「速く振り向くと物が数フレーム消える」は構造的に起きない）。★**ON にすると深度プリパスも強制的に走る**。TAA/SSAO/SSR/DXR のどれかが有効なシーンではプリパスは元々走っているので追加コストは `gpuPassMs.hiZ`（実測 0.04ms）だけだが、**どれも無効なシーンで ON にするとプリパスぶんの描画コールが増えて遅くなることがある**。GPU 律速のときに効く機能で、CPU 律速のシーンでは fps は改善しない。既定 OFF、`settings.json` の `"render_occlusion_culling"` に保存。実際に何体隠れたかは `dx12_perf_stats` の `occlusion` ブロック（`occluded`/`tested`/`ratio`/`predicatedDraws`/`batches`）を見ること。正射/2Dビューでは自動無効 |
| `dx12_vg_stats` | `{}` | `{enabled, active, valid, instances, instancesInFrustum, sourceTrisInFrustum, nodesVisited, clustersSelected, visibleClusters, phase2Clusters, trianglesDrawn, cullGpuMs, vramMB, overflow, levelHistogram[], culled{…}, hzb{…}, assets[{path,ready,pages,clusters,bvhDepth,sourceTriangles,vramMB}]}` ※**仮想ジオメトリ（Nanite 風）P2 の GPU カリング統計**。`VirtualGeometry` コンポーネント（`.vgeo` を置くと自動で付く）を持つエンティティを、GPU（compute・専用ルートシグネチャ）で インスタンス → BVH 走査 → クラスタ（画面空間誤差による LOD 選択 / 錐台 / 法線コーン / **HZB 二段**）とカリングした結果。`clustersSelected` = LOD 選択で選ばれた数、`visibleClusters` = 全部のカリングを通った数（一相目 + 二相目）、`phase2Clusters` = 二相目（今フレームの HZB）で救われた数、`trianglesDrawn` = 見かけの三角形数、`sourceTrisInFrustum` = 錐台内の LOD0 換算三角形数、`cullGpuMs` = Execute からラスタを除いた GPU 時間（カリング + HZB 再構築）、`executeGpuMs` = Execute 全体（`gpuPassMs.vgCull` と同じ区間）、`raster{active, usedAs, gpuMs, gpuMsPhase1/2, clusters, trianglesDrawn, trianglesHw/Sw, clustersHw/Sw}`（P3。Sw は未実装で 0）。`set_virtual_geometry {measure:true}` のとき `raster` に `overdraw` / `psInvocations` / `coveredPixels` / `msTrianglesOut` / `msSmallPrimCulled` / `asCulledClusters` / `edgePxHistogram{bins,clusters,triangles}`（可視クラスタの最長辺の画面長分布 = SW ラスタ判断用）が加わる。統計は約 3 フレーム遅れ。★**P3: メッシュシェーダ対応 GPU では VG 本体が可視性バッファ + 深度へ描かれ、プロキシは主ビューの深度プリパス / フォワードから外れる**（影・TLAS・ピッキング・物理はプロキシのまま）。最終シェーディング（材質）は P4 で、暫定は面法線ランバート。HZB は VG 自身の深度から作るので、プロキシ由来の自己遮蔽は解消する。`meshShaders` / `rasterWanted` / `hiddenProxies` も出る。★**P4（材質 resolve）**: VG 画素はフォワードと同じライティング（`ForwardShade.hlsli`）で塗られる。`resolve{supported, active, gpuMs, gbufferActive, gbufferGpuMs, mainRsBindless, assetsWithMaterials, assetsReady, appHeapDescriptors, sharedAppHeap}`（`gpuMs` = 材質 resolve の GPU 時間、`gbufferGpuMs` = VG 画素の速度 + G-Buffer パスの GPU 時間、`appHeapDescriptors` = VG がアプリの SRV ヒープに持つディスクリプタ数）/ `stableOrder{active, sortedClusters, skippedPhases}`（決定論の安定ソート）/ `vgGpuTotalMs`（カリング + ラスタ + HZB + G-Buffer + resolve）/ `ineligible{count, entities[{entity,name,reason}]}`（VG の対象外 = プロキシで描くエンティティと理由: スキンド / ノードアニメ / 地形 / カスタムシェーダー / 材質アセット・グラフ材質 / テクスチャ上書き / アルファクリップ・半透明 / UV アニメ）。`settings` に `resolve` / `stableOrder` が加わる |
| `dx12_set_virtual_geometry` | `{enabled?:bool, lodPixelError?:0.25..8, hzbCulling?:bool, coneCulling?:bool, instanceMinPx?:0..64, vramBudgetMB?:64..65536}` | `dx12_vg_stats` と同じ | ※仮想ジオメトリ（`Scene` の設定。シーン JSON の `virtualGeometry` に保存。既定値のときは何も出力しない）。**既定 OFF**。ON にすると深度プリパスが走り（HZB の入力）、カリングの統計だけが出る（**描かない**）。`lodPixelError` = LOD の画面空間誤差しきい値 τ（レンダー px。既定 1）。キューが溢れたら次フレームの τ を自動で 1.25 倍する（`tauUsed`）。`vramBudgetMB` を超える `.vgeo` は構造化エラー（`assets[].error`）で読み込みを拒否する。SM 6.6 + Resource Binding Tier 3 が無い GPU では無効（`gpuSupported:false`）。**P3 の追加（実行時のみ・シーンには保存しない）**: `raster`（既定 true。false = 統計だけ・プロキシを描く）/ `rasterAs`（増幅シェーダ経由。既定 false）/ `smallPrimCull`（画素中心を覆わない三角形 / クラスタを落とす。既定 false: 5060 の実測では速くならない）/ `measure`（断片数・オーバードロー・辺長ヒストグラムの計測。少し遅い）/ `forceLod0`（LOD0 の葉だけを選ぶ検証用。全 LOD0 の素朴な参照）。メッシュシェーダ非対応 GPU（`DX12_DISABLE_MESHSHADER=1` でも再現）では `raster` は自動で無効になりプロキシが描かれる。**P4 の追加（実行時のみ）**: `resolve`（既定 true = 材質 resolve。false = P3 の暫定シェーディング。A/B 用）/ `stableOrder`（決定論: 可視リストをフェーズごとに `{instance, clusterRef}` 順へ安定ソートしてからラスタ。既定 false。`screenshot_final {deterministic:true}` の間は自動で ON）。メイン RS にバインドレスが無い環境（`DX12_DISABLE_MAIN_BINDLESS=1` / SM 6.6 未満）では resolve が使えないので VG は描かずプロキシが描かれる |
| `get_gi_mode` | `{}` | `{mode:"legacy"|"new", debugStage, ddgiEnabled, ddgiActive}` | ※**GI モード（シーン単位。GI_FOUNDATION_DESIGN）**。`legacy` = 従来（キー無しの既存シーン。DDGI もフォワードも SSGI も絵は 1 ビットも変わらない）/ `new` = 空の遮蔽つき GI（DDGI のプローブ分類・再配置・RTXGI 流の補間・空の可視率、ヒット面の多重バウンス（空はミスしたレイからだけ）、フォワードの拡散環境光を DDGI が置換 + 鏡面の空遮蔽、SSGI のミスは DDGI）。`new` でも DDGI の ON と部屋を覆う格子は別に要る（`set_dxr ddgiEnabled` / `ddgiProbeCount*` / `ddgiOrigin*` / `ddgiSpacing`）。`ddgiActive:false` の間は IBL のまま。MCP 専用（TS 専用ツールは無い。`dx12_call {name:"get_gi_mode"}`） |
| `set_gi_mode` | `{mode:"legacy"|"new", debugStage?:0..15}` | `get_gi_mode` と同じ形 + `applied` | ※シーン JSON の `gi.mode` に保存（`legacy` は書かない）。Undo で戻る。変更で DDGI の履歴を捨てて埋め直す。`debugStage` は検証用ビット（保存しない）: bit0 旧の空の項 / bit1 再配置なし / bit2 全プローブ有効 / bit3 最寄りプローブの状態を色で表示（赤 = 無効 / 緑 = 有効 / 青 = オフセット）。評価は `tools/parity/gi_eval.ps1`（`--capture-mode det` が既定。★free は実測で DDGI の多重バウンスが効かない絵になる（撮影時に履歴が捨てられている疑い。原因は未確定）ので DDGI 構成には使えない） |
| `migrate_gi` | `{to:"new"\|"legacy", autoFit?:true}` | `{to, applied, reason, mode, gridFitted, gridFromGeometry, ambientChanged, ddgi:{enabled, probeCount[3], probeTotal, spacing, origin[3], bounceIntensity}, ssgiEnabled, rtShadowEnabled}` | ※**GI の旧 ⇄ 新をまとめて切り替える**（S5。エディタのライティング窓「新しい GI に切り替える / 旧に戻す」・新規シーンの既定と同じ関数 `scene/GiMigration.h`）。`to:"new"` = `gi.mode=new` + DDGI ON（格子を**動かない MeshRenderer のバウンディングボックス**へ自動フィット: 間隔 1.0 m から 0.25 m 刻みで広げ、軸 32 個・総数 4096 個以内、範囲の外へ 0.5 間隔張り出し、縦は 4 m 以上・横は 8 m 以上を確保、水平 100 m 超の巨大な床 / 動く剛体は無視。ジオメトリが無ければ ±8 m の既定）+ 多重バウンス 1.0 + 全太陽の `ambient=0` + SSGI ON + RT 影 ON。`autoFit:false` は DDGI が既に ON なら今の格子（手置き）を残す（OFF ならフィットする）。**GI S4: 既定の格子はカメラ追従**（`volume:"follow"`・既定。1 カスケード 22×8×22・近 0.8 m / 遠 2.0 m の 2 カスケードがカメラに合わせて間隔単位でスナップして動く。履歴ははみ出した列だけ捨てる。1 フレームの DDGI は `ddgiBudgetMs`（既定 1 ms）に収まるよう更新プローブ数を間引く）。`volume:"scene"` は従来のシーン AABB への固定ボリューム。`set_dxr ddgiFollowCamera/ddgiSpacing1/ddgiBudgetMs` は GI モード new のときだけ効く（legacy は固定ボリュームのまま）。`to:"legacy"` = `gi.mode=legacy` + DDGI OFF + `ambient` が 0 の太陽は 0.25（SSGI・RT 影は触らない）。**呼び出し 1 回 = Undo 1 エントリ**（全項目が戻る）。DXR 非対応 GPU の `new` は何も変えず `applied:false` + `reason`。`set_gi_mode` は「モードだけ」の低レベル口のまま（GI 評価ハーネスが構成を自前で組むため既定を変えていない）。MCP 専用（`dx12_call {name:"migrate_gi"}`）。**新規シーン**（`new_scene` / エディタの新規 / ランチャーの 3D テンプレート empty・fps・tps）は既定で `new` + 上の構成（DXR 非対応なら旧。2d テンプレートは正射カメラのスプライト主体で DDGI の利得が無いので旧のまま）。既存シーン（gi キー無し）は旧のまま開き、保存しても gi キーを書かない |
| `dx12_set_occlusion` | `{enabled:bool}` | `{enabled, active, ready, pyramid{width,height,mips}}` ※Hi-Z オクルージョンカリングの ON/OFF。★ON にすると深度プリパスも強制的に走る。TAA/SSAO/SSR/DXR のどれかが有効なシーンでは追加コストは Hi-Z ぶん(実測 0.04ms)だけだが、どれも無効なシーンで ON にするとプリパスぶんの描画コールが増えて逆に遅くなることがある。GPU 律速のときに効く機能で、CPU 律速のシーンでは fps は改善しない。既定 OFF、`settings.json` の `render_occlusion_culling` に保存される |

> **TAA の効果確認は `dx12_ui_screenshot` を使うこと。** `dx12_screenshot` はポスト前の `m_sceneRT` を読むので、TAA の解決結果も `debugVelocity` の可視化も写らない（どちらもその後段で出力される）。

### 4-2-1. `dx12_render_debug` の mode 一覧

「絵が変」の原因を切り分けるための唯一の入口。**呼ぶ前と後でシーンの設定は完全に同じ**（一時的に ON にした機能は必ず戻す）。
可視化はポスト前の `m_sceneRT` へ描くので、返ってくる PNG には**必ず写る**（`dx12_screenshot` の B5 の罠を踏まない）。
`toneMapped:false` のモードはトーンマップ/露出を掛けずに 8bit へ落とすので、**PNG のピクセル値がそのままバッファの値**として読める。

| mode | 出るもの | 実装 | 備考 |
|---|---|---|---|
| `normal` | ワールド法線 `0.5+0.5*N`（+X=赤 +Y=緑 +Z=青） | 専用パス | **G-Buffer は幾何法線**。法線マップは載っていない（SSR/SSGI が見ているのもこれ） |
| `roughness` | G-Buffer の roughness（白 = 1） | 専用パス | **スカラー値のみ**。ORM テクスチャは載っていない |
| `metallic` | G-Buffer の metallic（白 = 1） | 専用パス | 同上 |
| `depth` | ビュー空間 Z をヒートマップ（青=近 → 赤=遠、空は黒） | 専用パス | `depthRange`(m) で正規化。既定 100 |
| `ao` | SSAO（白 = 遮蔽なし） | 専用パス | SSAO を一時 ON |
| `contactShadow` | コンタクトシャドウ（白 = 遮蔽なし） | 専用パス | コンタクトシャドウを一時 ON |
| `velocity` | 速度バッファ（R=+X G=+画面下、**静止していれば一様な (0.5,0.5,0.5)**） | 専用パス | `gain` で強調（既定 1、20 くらいが見やすい） |
| `ssr` | SSR の結果（リニア HDR にガンマのみ） | 専用パス | SSR を一時 ON。時間蓄積があるので `frames` を 8〜16 に |
| `ssgi` | SSGI の結果 | 専用パス | 同上 |
| `rt` | **DXR のプライマリレイのヒット距離**をヒートマップ（空/ミスは黒） | 専用パス | TLAS が正しく建っているかの目視確認。`depthRange`(m) で正規化。RT 影 / RT-AO が OFF でも TLAS を一時的に建てる |
| `rtDiff` | **&#124;RT のヒット距離 − ラスタの距離&#124;** をヒートマップ（**黒 = 完全一致**、マゼンタ = 片方だけヒット） | 専用パス | ★加速構造の検証はこれが本命。行列の転置ミス / ノード変換の付け忘れを一発で炙り出す。`gain` を 20 くらいにすると 5cm でフルスケール。**スキンドと半透明は TLAS に入らない仕様なのでマゼンタになる**。BLAS は LOD0 固定なので、遠くて低 LOD で描かれている物は数 cm の差が出るのが正常 |
| `shadowCascade` | CSM のカスケードを色分け（赤/緑/青/黄の色掛け） | 既存 `shadowParams.w` | フォワード PS の既存実装をそのまま使う |
| `lightComplexity` | クラスタごとのライト数ヒートマップ（青0 → 緑 → 赤、**白 = 128 灯で切り捨て中**） | 既存 `clusterExtra.z=1` | 正射/2D ではクラスタード自体が無効 |
| `clusterGrid` | クラスタ境界の市松 | 既存 `clusterExtra.z=2` | |
| `decalCount` | クラスタごとのデカール枚数ヒートマップ（**白 = 16 枚で切り捨て中**） | 既存 `clusterExtra.z=3` | デカール 0 枚のクラスタはほぼ黒 |
| `fogScattering` / `fogTransmittance` / `fogSlice` | ボリュメトリックフォグの散乱 / 透過率 / froxel スライス | 既存 `FogParams.gMisc.z` | フォグが無効なら何も出ない（`warnings` で通知） |
| `vgCluster` / `vgTri` | **仮想ジオメトリ（P3）の可視性バッファ**: クラスタごと / 三角形ごとの色（`instance + clusterRef` のハッシュ。描画順に依らない）。VG でない画素は 80% 暗くする | `VirtualGeometrySystem::DrawDebug` | VG が ON かつ `raster:true` かつメッシュシェーダ対応 GPU のときだけ絵が出る（それ以外は `warnings`）。`toneMapped:false` |
| `vgLod` | VG の LOD レベル（青 = 0 … 赤 = 最粗） | 同上 | LOD の切り替わり（ポップ / 遷移帯）の目視確認 |
| `vgDepth` | VG 込みの深度（白 = 近、`depthRange`(m) で黒） | 同上 | 非 VG の深度（プリパス）と VG の深度が同じバッファに載る |
| `vgOverdraw` | 断片数（深度テストを通った数）のヒートマップ（青 = 1 … 赤 = 8+） | 同上（計測モードが自動で ON） | 描画順に依存して毎フレーム少し揺れる。`vg_stats.raster.overdraw` に平均が出る |
| `vgCoverage` | 被覆: 緑 = VG / 灰 = 非 VG のジオメトリ / 黒 = 空 | 同上 | 穴（物が消える）の検出用。VG の輪郭内に黒（空）が出たら穴 |
| `vgMaterial` | 材質（アセット + 材質番号のハッシュ色） | 同上 | P4。1 クラスタ = 1 材質の確認。材質境界がクラスタ境界に沿う |
| `vgMip` | resolve がアルベドに使うミップ段（解析 UV 勾配。青 = 0 … 赤 = 10 以上 / 灰 = テクスチャ無し） | 同上 | P4。遠く / 斜めほど赤くなれば勾配が正しい（可視性バッファ方式は ddx が壊れるので解析的に作っている） |
| `vgTileMaterials` | 8x8 タイル内の異なる材質の数（1 = 緑 … 4 以上 = 赤） | 同上 | P4。赤いほど resolve の波面が分岐（材質のタイル分類の要否の判断材料） |
| `vgNormal` | resolve が使う頂点法線（補間・ワールド。0.5 + 0.5n） | 同上 | P4。法線の復元（oct16 の復号 + 補間）の確認 |
| `off` | 何もせず全部を戻すだけ（スクショも撮らない） | — | 途中で失敗したときのリセット用 |

**非対応（作っていない。理由つき）**
- `albedo` … 前方レンダラなのでアルベドの G-Buffer が存在しない。作るには深度プリパスに RT をもう 1 枚足す必要があり、
  速度 PSO の RTV 本数（＝00-COORDINATION §5.5 の契約）に手を入れることになるので見送った。
- `overdraw` … 加算カウント用の専用パス（全メッシュを再描画してブレンド加算）が要る。既存のどのバッファにも無い。

**⚠️ `normal` / `roughness` / `metallic` / `velocity` は「深度+速度プリパス」でしか書かれない**ので、TAA も SSR も SSGI も
OFF のときは TAA を一時的に ON にして撮る（`warnings` に出る）。この 4 モードが「ジオメトリだけの粗い絵」に見えるのは仕様。
| `dx12_undo` | `{onlyAi?:bool=true}` | `{undone, wasAi, onlyAi, undoable, willUndo, next:{undo, redo}}`(互換で `queuedUndo`) ※遅延応答。**MCP の編集は 1 呼び出し = 1 エントリ「AI: <method>」として積まれる**(トランザクション中は「AI: <label>」の 1 エントリ)。★`onlyAi` は既定 true: 一番上が人の編集なら戻さずに MODE_CONFLICT(3)+ヒント(人の作業を黙って消さない)。人の編集ごと戻すときだけ `onlyAi:false` を明示。トランザクションが開いている / Play 中も MODE_CONFLICT |
| `dx12_redo` | `{onlyAi?:bool=true}` | `{redone, wasAi, onlyAi, redoable, willRedo, next}`(互換で `queuedRedo`) ※遅延応答。onlyAi は undo と同じ |
| `dx12_transaction_begin` | `{label?:string}` | `{open, label, entryName, undoDepth, idleTimeoutSec}` ※以降の MCP の編集を 1 エントリ「AI: <label>」へまとめ始める。入れ子・Play 中は MODE_CONFLICT。開いている間の MCP の play / open_scene / new_scene / open_project は断られる。人の Play・シーン切り替え・Ctrl+Z か 600 秒放置で「確定扱い」に自動で閉じる |
| `dx12_transaction_commit` | `{}` | `{committed, label, calls, pushed, entryName, humanEditsDuringTransaction, top}` ※遅延応答。1 エントリとして積んで閉じる(undo 1 回で丸ごと戻せる)。応答前に次の書き込みを送ると MODE_CONFLICT |
| `dx12_transaction_rollback` | `{}` | `{rolledBack, label, calls, humanEditsDuringTransaction, top, sceneGeneration}` ※遅延応答。begin 以降の MCP の編集を全部逆順に戻す(消した物は guid ごと復元)。人の編集は戻さない |
| `dx12_transaction_status` | `{}` | `{open, label?, calls?, callNames?, ageSec?, idleSec?, autoCloseInSec?, humanEditsDuringTransaction?, closePending, lastClosed, top, undoDepth, redoDepth, mode}` ※commit / rollback が「開いていない」で弾かれたら `lastClosed.reason` を見る |
| `dx12_save_scene` | `{path?:string}` | `{path}` ※省略で現在シーンへ上書き |
| `dx12_create_lua_component` | `{name:string, code:string}` | `{path}` ※書込前に構文検証。既存パスなら上書き更新も兼ねる |
| `dx12_create_shader` | `{name:string, code:string}` | `{path, compiled, error?}` ※assets/shaders/に作成/上書き後、即コンパイルを試す。Luaと違い失敗してもファイルは残る(反復修正前提) |
| `dx12_attach_lua_component` | `{entity:int, script:string(assets相対)}` | `ok` |
| `dx12_set_lua_property` | `{entity?/name?, key:string, value:any}` | `{entityId, key, value}` ※スクリプトの `properties` 宣言にあるものだけ。Playing 中は即再注入（`OnStart` 再実行）、Editor 中は保存だけで次 Play から反映 |
| `dx12_reload_scripts` | `{path?:string}` | `{reloaded, cleared}` ※実行時エラーで死んだスクリプトを **Play を止めずに**復帰させる。ファイルを書き換えた場合は 0.5 秒で自動リロードされるので不要 |
| `dx12_reload_assets` | `{path?:string(assets相対のファイル or フォルダ), force?:bool=false}` | `{path, force, reloaded, textures[], models[], reboundEntities, checkedTextures, checkedModels, skipped[], warnings[], note}` ※**DCC ツールと行き来するときの必須ツール**。エンジンはテクスチャもモデルも**プロセス起動から一生キャッシュする**ので、Blender や画像編集ソフトから同じパスへ書き出し直しても絵は変わらない（これが無いと確認のたびにエディタ再起動＝1 往復 20 秒以上）。★**シーンは開き直さない**: エンティティ / Transform / 選択状態はそのまま、いま置かれている `MeshRenderer` の参照だけが新しい実体へ張り替わる（`reboundEntities` がその体数）。テクスチャは**同じ `Texture` オブジェクト・同じ SRV 番号**のまま中身だけ差し替わるので、スプライト / UI / マテリアルの参照は 1 つも直さずに済む。★既定はディスクの更新時刻を見て**変わったものだけ**。書き出し直したのに `reloaded` が 0 なら `force:true`。★キューブマップ / 配列テクスチャ（スカイボックス・地形レイヤー）は張り直せず `skipped` に載る（シーンを開き直すこと）。モデル内の埋め込みテクスチャはモデル側を読み直せば一緒に更新される |
| `dx12_set_color` | `{entity?/name?, color:[r,g,b]}` | `{entityId, color}` ※メッシュの基本色（頂点色の乗算）。金属感は `dx12_set_pbr` と併用 |
| `dx12_install_font` | `{family:string, weight?:int=400}` | `{fontPath, family, weight}` ※Google Fonts から `.ttf` を `assets/fonts/` へ取り込む。★日本語 UI には日本語対応フォント（Noto Sans JP 等）を選ぶこと（欧文フォントは豆腐になる） |
| `dx12_create_prefab` | `{entity:int, path?:string}` | `{path, entityId}` ※path省略で assets/prefabs/<name>.prefab |
| `dx12_eval_lua` | `{code:string}` | `{result:string}` ※任意 Lua をその場実行(デバッグ用) |
| `dx12_build_game` | `{waitSec?:number(0..110), wait?:bool}` | 既定は裏ジョブで始めて即応答 `{started, state:"running", stage, stageName, pct, ...}`(エディタは固まらない)。`waitSec` で終わるまで最大その秒数だけ待つ。二重起動は `started:false`。ビルド中に保存していない編集は配布物に入らない(ディスクの内容が対象)。`wait:true` は従来の同期(`{success, outputDir, error?}`・エディタが固まる) |
| (engine) `get_build_status` | `{}` | `{state: idle\|running\|succeeded\|failed\|cancelled, stage(1..5), stageName, pct, indeterminate, done/total, elapsedSec, detail, outputDir, error?}` ※`dx12_call {name:"get_build_status"}`。テクスチャの事前生成の段は進みが読めないので `indeterminate:true`(pct は段の始まりの値) |
| (engine) `cancel_build` | `{}` | `{requested}` ※`dx12_call {name:"cancel_build"}`。段の切れ目・ファイルごとに止まり、途中の出力フォルダは消す |
| `dx12_set_texture` | `{entity:int, path:string(assets相対、空文字で解除), slot?:"albedo"\|"normal"\|"metalRoughness"\|"emissive", submesh?:int}` | `{entityId, slot, submesh, path}` ※Inspector のテクスチャ D&D と同じインスタンス単位 override(Material 共有を壊さない)。`emissive` は貼っただけでは光らない（色×強度が既定 0）ので `dx12_set_pbr` の `emissiveIntensity` を一緒に上げること |
| `dx12_play_anim` | `{entity:int, clip?:int, clipName?:string, blend?:f=0.3, loop?:bool, speed?:f, state?:string, layer?:int=0}` | `{entityId, clip, clipName, blend, speed}` または `{entityId, state, layer, blend}` ※スケルタルアニメのクロスフェード再生(Lua playAnim と同経路)。**`state` を渡すと `.animfsm` のステート遷移**になる(`AnimatorController` が要る)。渡さなければ従来どおり clip 経路で完全後方互換。`state`/`layer` は **TS 側未定義** |
| `dx12_set_anim_param` | `{entity?:int / name?:string(エンティティ名), param:string(FSM パラメータ名), value?:number\|bool, trigger?:bool}` | `{entityId, param, name(=param。後方互換), value}` ※アニメ FSM のパラメータを外から叩いて遷移を検証する。**★パラメータ名は `param`**。`name` は他ツールと同じく「エンティティ名」。`param` を省略したときだけ `name` をパラメータ名として読む後方互換がある（`{entity, name:"Speed"}` も通る）が、**新しいコードは必ず `param` を使うこと** |
| `dx12_net_setup` | `{role:"host"\|"client"\|"offline", address?:string, port?:int}` | `{testRole, address, port}` ※次の `dx12_play` で自動 Host/Join(ツールバーの Play ロールと同じ) |
| `dx12_net_launch_test_client` | `{}` | `{requested}` ※ホスト Playing 中のみ。同エンジンをもう1プロセス起動し 127.0.0.1 へ自動接続(フレーム境界) |
| `dx12_set_editor_camera` | `{position?:[x,y,z], target?:[x,y,z], yawDeg?:f, pitchDeg?:f, release?:bool}` | `{position, forward, yawDeg, pitchDeg, overridden, mode, note}` ※シーンビューのカメラを任意視点へ。target 指定で yaw/pitch 自動逆算。**Play 中も使える**: Playing 中に呼ぶとアクティブ `CameraComponent` の毎フレーム同期を止めて視点を固定する（`overridden:true`）。`{"release":true}` でゲームカメラへ返す。**Play/Stop の遷移でも自動解除**。Play 中の絵で `look_compare` / `camera_path` を回すための機能 |
| `dx12_look_at` | `{entity:int, target?:[x,y,z], targetEntity?:int, targetName?:string, upright?:bool}` | `{entityId, rotation, target}` ※+Z 正面の想定で rotation(Euler) を書く。upright=true でピッチ0。親が回転してると厳密でない |
| `dx12_snap_to_ground` | `{entity:int, offset?:f, precise?:bool=true}` | `{groundY, movedBy, method:"raycast"\|"aabb", position, groundEntityId?}` ※**三角形精密レイキャストで真下の実際の面へ接地**(地形の起伏・斜面・彫った岩に乗る)。真下に三角形が無ければ従来の AABB 天面判定へフォールバック(`method:"aabb"`)。床なしは y=0。Editor 中でも動く |
| `dx12_import_asset` | `{sourcePath:string(絶対パス可), destPath:string(assets相対), overwrite?:bool}` | `{imported:[相対パス...], count}` ※外部ファイル/フォルダを assets へコピー。.gltf はフォルダごと |
| `dx12_move_asset` | `{from, to, overwrite?:bool}` | `{from, to, note}` ※assets 内の移動/リネーム。**シーン内の参照パスは自動更新されない** |
| `dx12_delete_asset` | `{path, recursive?:bool}` | `{deleted, removedCount, wasDirectory}` ※ディレクトリは recursive:true 必須。参照中アセットを消すと壊れる |
| `dx12_terrain_generate` | `{entity?/name?, preset?:"hills"\|"canyon"\|"mountains", seed?:int, frequency?, octaves?, amplitude?, ridged?, baseHeight?, edgeFalloff?, valleyDepth?}` | `{entityId, preset, params:{...}, minHeight, maxHeight, resolution, worldSize}` ※高さ配列を丸ごと作り直す(既存の彫りは消える)。**同じ seed/params なら毎回同じ地形=冪等**。★Editor 限定 |
| `dx12_terrain_sculpt` | `{entity?/name?, brush?:"raise"\|"lower"\|"smooth"\|"flatten"\|"noise", point?:[x,z] \| points?:[[x,z]...](最大512) \| worldPos?:[x,y,z], radius?=12, strength?=5, falloff?=0.5, flattenHeight?, mirrorX?, mirrorZ?, noiseFrequency?, noiseOctaves?, noiseRidged?, seed?}` | `{entityId, brush, points, radius, strength, changed, minHeight, maxHeight}` ※座標は**ワールド XZ**。相対操作(2回撃つと2回ぶん)。`flatten`+`flattenHeight` は絶対値なので冪等寄り。★Editor 限定 |
| `dx12_terrain_erode` | `{entity?/name?, iterations?:int=16, talusDeg?:f=34, region?:[minX,minZ,maxX,maxZ]}` | `{entityId, iterations, talusDeg, changed, minHeight, maxHeight}` ※熱浸食。相対操作。★Editor 限定 |
| `dx12_terrain_paint` | `{entity?/name?, layer?:0..3, point?:[x,z] \| points?:[[x,z]...](最大512) \| worldPos?:[x,y,z], radius?=12, strength?=0.7, falloff?=0.5}` | `{entityId, layer, points, radius, strength, changed, splatSize}` ※**テクスチャレイヤーの重み**を円ブラシで塗る。座標は**ワールド XZ**。相対操作。`terrain.layerSetPath` 未設定だと `INVALID_PARAM`。★Editor 限定 |
| `dx12_terrain_autopaint` | `{entity?/name?, rockSlopeStart?, rockSlopeEnd?, dirtSlopeStart?, dirtSlopeEnd?, snowHeightStart?, snowHeightEnd?, noiseStrength?}` | `{entityId, splatSize}` ※傾斜と標高から 4 層を焼き直す（**冪等**。手で塗った内容は消える）。傾斜は 0=平ら〜1=垂直、標高はワールド Y(m)。★Editor 限定 |
| `dx12_terrain_set_layers` | `{entity?/name?, layerSetPath:string(assets 相対 .terrainlayers。**空文字で割当解除**), splatResolution?:int=512(32..2048), autopaint?:bool=true, uvScale?, heightBlendDepth?:0.01..1, triplanarSharpness?:1..16, normalStrength?:0..2, macroScale?:10..400, macroStrength?:0..1, distTilingStart?:5..200, distTilingFarScale?:2..16, pomHeightScale?:0..0.3, pomFadeStart?:0..40, pomFadeEnd?:1..120, triplanar?:bool, pom?:bool, macro?:bool, distTiling?:bool}` | `{entityId, layerSetPath, previousLayerSetPath, layerCount, layerNames:[...], splatPath, splatSize, splatCreated, uvScale, terrainMatFlags, sceneGeneration, note}` ※**地形にテクスチャレイヤーを割り当てる唯一の MCP 経路**（#27）。初回割当時にスプラットを作り、`autopaint:true`（既定）なら傾斜/標高から自動で塗る。省略したパラメータは触らない（冪等）。`layerSetPath:""` で外すと従来の頂点色描画へ戻る。★Editor 限定 |
| `dx12_terrain_splat_info` | `{entity?/name?, gridSize?:int=8(0..32。0 で grid を返さない), point?:[x,z] \| points?:[[x,z]...](最大256)}` | `{entityId, layerSetPath, splatPath, hasSplat, unsavedSplat, splatSize, coverage:[4](層ごとの平均重み 0..1), dominantRatio:[4](その層が最大だったテクセルの割合), gridSize, grid:[gridSize 本の文字列。`grid[z][x]` が `'0'..'3'` でそのセルの支配レイヤー。z が増えると +Z、x が増えると +X], samples:[{world:[x,z], texel:[tx,tz], weights:[4], dominant:int}], note}` ※**読み取り専用**。`terrain_paint` / `autopaint` の結果を絵を見ずに検証する。スプラット未作成なら `hasSplat:false` と案内だけ返す。Playing 中も可 |
| `dx12_sculpt_brush` | `{entity?/name?, brush?:"draw"\|"pull"\|"push"\|"smooth"\|"flatten"\|"pinch"\|"noise"\|"grab", position?:[x,y,z](ワールド) \| localPosition?, radius?=0.5, strength?=0.2, falloff?=0.5, direction?, grabDelta?, symmetryX/Y/Z?, noise*?, seed?}` | `{entityId, brush, movedVertices, localCenter, radius, strength, vertexCount, triangleCount, localBounds}` ※radius/strength は**メッシュのローカル単位**(Transform の scale が掛かる前)。相対操作。★Editor 限定 |
| `dx12_set_sun` | `{timeOfDay?:0..24, azimuth?:deg, elevation?:deg, color?:[r,g,b], kelvin?:1000..40000, intensity?, ambient?}` | `{entityId, name, direction, azimuthDeg, elevationDeg, color, intensity, ambient, timeOfDay}` ※最初の DirectionalLight を**絶対指定**で更新(冪等)。方位/高度は「太陽が見える方向」(+Z=0°, +X=90° / 高度 0=地平線)。物理大気が ON のとき `timeOfDay` は大気の時刻を設定し(応答に `atmosphere`)、`azimuth`/`elevation` は `sunMode=1`(向きを直接指定)へ切り替える |
| `dx12_navmesh_build` | `{cellSize?, cellHeight?, agentHeight?, agentRadius?, agentMaxClimb?, agentMaxSlope?, minRegionArea?, mergeRegionArea?, maxEdgeLen?, maxSimplificationErr?, maxVertsPerPoly?:int, monotonePartition?:bool, filterLedgeSpans?:bool, filterLowHanging?:bool, useBounds?:bool, boundsMin?:[x,y,z], boundsMax?:[x,y,z]}` | `{ok, settingsChanged, stats:{...}, config:{...}, stageLog}` ※シーンのメッシュを**実際の三角形のまま**ボクセル化して歩ける面を取り出す。引数はシーンの生成設定を上書きしてから焼く。★Editor 限定。焼いた実体は隣の `.nav`（`dx12_save_scene` で書かれる） |
| `dx12_navmesh_settings` | build と同じキー | `{applied, config, note}` ※焼き直さずに設定だけ変える。引数なしで現在値を読める |
| `dx12_navmesh_info` | `{}` | `{config, stats:{built, polyCount, vertCount, sampleCount, gridW, gridH, walkableArea, buildMs, memoryBytes, boundsMin, boundsMax}, debugDraw}` |
| `dx12_navmesh_path` | `{from:[x,y,z], to:[x,y,z], searchRadius?, searchHeight?}` | `{pointCount, points:[[x,y,z]...], length, reached}` ※A* + ファネル。`reached:false` は「到達できないので一番近い所まで」 |
| `dx12_navmesh_sample` | `{point:[x,y,z], searchRadius?, searchHeight?}` | `{onNavMesh, poly?, point?, distance?}` ※位置を歩行面へ落とす（高さは坂道でもボクセル分解能で正確） |
| `dx12_navmesh_raycast` | `{from:[x,y,z], to:[x,y,z], searchRadius?, searchHeight?}` | `{hit, t, point, normal}` ※壁（隣のポリゴンが無い辺）との精密な交差。「真っ直ぐ行けるか」の判定 |
| `dx12_navmesh_debug` | `{enabled?:bool}` | `{enabled}` ※シーンビューにワイヤを重ねる（明るい線=壁 / 暗い線=ポータル）。MCP のスクショにも写る |
| `dx12_navmesh_clear` | `{}` | `{cleared}` ※★Editor 限定 |
| `dx12_apply_lighting_preset` | `{preset:"day"\|"dusk"\|"night"\|"indoor"\|"horror"\|"studio"}` | `{preset, label, tip, sun:{...}\|null, post:{exposure/bloom/vignette/saturation...}}` ※**エディタの「ライティング」窓と同じ表・同じ式**(`src/editor/LightingPresets.h` に 1 本化)。太陽が無ければポストのみ適用 |

### 4-3. 生成・削除・モード遷移(遅延同期 — 本物の値が返る)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_create_entity` | `{type:"box"\|"sphere"\|"plane"\|"empty"\|"camera"\|"light_directional"\|"light_point"\|"light_spot"\|"particle_emitter"\|"trigger"\|"ui_canvas"\|"ui_image"\|"ui_text"\|"ui_button"\|"ui_slider"\|"ui_toggle"\|"ui_scrollview", name?, position?:[x,y,z], parent?:int, parentName?:string, idempotency_key?:string}` | `{entityId, name, sceneGeneration}` ※light_*/camera/particle_emitter/trigger は既定値で生成(dx12_set_component で調整)。ui_* はエディタと同じ部品構成で生成され `entityIds`(自動Canvas/ラベル子含む全id)も返る。parent/parentName は ui_*(ui_canvas 以外)の親指定 |
| `dx12_spawn_box` | `{name?, position?, scale?, rotation?, color?, metallic?, roughness?}` | `{entityId, name, sceneGeneration}` ※足場/壁/床用。内部で create_entity → set_transform → set_pbr → set_color を順に実行する |
| `dx12_spawn_sphere` | `{name?, position?, scale?, rotation?, color?, metallic?, roughness?}` | `{entityId, name, sceneGeneration}` |
| `dx12_spawn_coin` | `{name?, position?}` | `{entityId, name, sceneGeneration}` ※金色の薄い円盤 + tag `coin`。回転やスコア加算は Lua / Trigger で付ける |
| `dx12_spawn_model` | `{path:string(.gltf/.glb/.fbx/.obj), position?:[x,y,z], name?, idempotency_key?:string}` | `{entityId, name, sceneGeneration}` |
| `dx12_spawn_prefab` | `{path:string(.prefab), position?, name?}` | `{entityId, rootEntityId, entityIds:[...], name, sceneGeneration}` |
| `dx12_duplicate_entity` | `{entity:int}` | `{entityId, name, sceneGeneration}` |
| `dx12_delete_entity` | `{entity:int}` | `{deletedEntityId, deletedCount, sceneGeneration}` |
| `dx12_open_scene` | `{path:string(assets相対)}` | `{sceneName, path, entityCount, sceneGeneration}` |
| `dx12_open_project` | `{path:string(プロジェクトルート絶対パス)}` | `{name, rootDir, defaultScene, loading:true}` ※ロードは非同期に数フレーム進む。完了は `dx12_ping` の currentScene で確認 |
| `dx12_new_scene` | `{savePath?:string}` | `{applied}` |
| `dx12_play` | `{}` | `{mode:"Playing", sceneGeneration}` |
| `dx12_stop` | `{}` | `{mode:"Editor", sceneGeneration}` |
| `dx12_terrain_create` | `{name?="Terrain", resolution?:int=128(16..512), worldSize?:f=200, maxHeight?:f=200, position?:[x,y,z], uvScale?, color?:[r,g,b]}` | `{entityId, name, created, resolution, worldSize, maxHeight, sceneGeneration}` ※**同名があれば作り直さず設定更新**(冪等)。resolution/worldSize を変えた時だけ高さがリセットされ `heightsReset:true` |
| `dx12_sculpt_create` | `{name?="Sculpt", primitive?:"box"\|"sphere"\|"plane"\|"cylinder", subdivisions?:int=16(1..64), size?:f=2, position?, uvScale?, color?, collision?}` | `{entityId, name, created, vertexCount, triangleCount, sceneGeneration}` ※**同名があれば素体を作り直さない**(彫った形を失わないため) |
| `dx12_sculpt_make_editable` | `{entity?/name?(元モデル), name?(出力名。既定 "<元>_Sculpt")}` | `{entityId, name, created, sourceEntityId, vertexCount, triangleCount, sceneGeneration}` ※元の .glb 等は読むだけ。CPU 頂点キャッシュが無いモデルは不可 |

### 4-4. Node 合成ツール(エンジン非依存。Node が複数 call を順に実行)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_batch` | `{ops:[{method:string, params:object}], atomic?:bool=true, label?:string, stopOnError?:bool}` | `{results:[{index, ok, result?, error?, error_code?, skipped?}], transaction?:{label, committed?\|rolledBack?, calls, entryName?, note}}` ※`atomic`(既定)はトランザクションで包む: 失敗したらそこで止めて begin 前へ丸ごと戻し、成功したら Undo 1 エントリにまとめる |
| `dx12_focus_and_screenshot` | `{entity:int}` | 画像コンテンツ(PNG) |

| `dx12_scatter` | `{type\|model\|prefab(どれか1つ), count:int(1..200), area:[minX,minZ,maxX,maxZ], y?:f, placement?:"random"\|"grid", seed?:int, randomYaw?:bool, scaleRange?:[min,max], snapToGround?:bool, namePrefix?:string}` | `{entities:[{entityId,name}], count, seed, placement, errors?}` |
| `dx12_screenshot_from` | `{position:[x,y,z], target?:[x,y,z]}` | 画像コンテンツ(PNG) ※Editor 限定 |
| `dx12_material_apply` | `{entity?/name?, dir?:string(assets相対), baseColor?, normal?, orm?, height?, uvScale?}` | `{entityId, applied:{...}, ignored:[{file,reason}]}` ※PBR の 4 点セットを 1 回で割当（`set_texture`×3 + `set_pbr` を畳んだもの）。`dir` を渡すとファイル名から用途を推定する（Poly Haven の `diff`/`nor_gl`/`arm`/`disp`、`albedo`/`basecolor`/`ORM`/`RMA` 等）。推定できなかったものは `ignored` に理由付きで返る |
| `dx12_scene_write` | `{path:string, scene:object, open?:bool}` | `{path, entityCount, opened}` ※**シーン JSON を直接書く**。MCP の spawn は 1 体につき 1 フレームかかる（遅延同期）ので、数十体以上を一気に並べるならこちらが桁違いに速い。書く前に `meshRenderer.modelPath` / `luaScript.scriptPath` の実在を `dx12_list_assets` と突き合わせて検証する |
| `dx12_look_compare` | `{referencePath:string, bins?:int=24, ...}` | 横並び PNG + **測光の数値**（対数輝度ヒストグラムと EMD / 平均・中央輝度 / コントラスト / 相関色温度 CCT / 平均彩度 / 黒潰れ率 / 白飛び率）と「どのノブをどっちへ何倍動かすか」の指示 ※リアル系ライティングを詰める本体 |
| `dx12_ui_compare` | `{referencePath:string, grid?:bool}` | 横並び PNG（左=参照 / 右=現在）+ `diffRatio(%)` ※`grid=true` で右側に 8px グリッドを重畳。**「参照と違う点を 3 つ」挙げてから直す**ループを回す用 |
| `dx12_camera_path` | `{path:[[x,y,z],...] または keyframes, shots?:int, source?:"backbuffer"\|"sceneRT", ...}` | 連写をタイル化した 1 枚（コンタクトシート）※静止画 1 枚では分からない TAA のゴースト / LOD ポップ / 影のちらつき / カリング抜けを探す用。既定は `screenshot_final`（TAA の解決結果はポスト前の RT に出ない） |
| `dx12_preview_model` | `{path:string(.gltf/.glb/.fbx/.obj)}` | 画像コンテンツ(PNG) ※一時 spawn→撮影→削除。シーンは変更されない |

**`dx12_batch` 実装**: 各 op を順に await。★`atomic:true`(既定)は `transaction_begin` → 実行 → どれかが失敗したらそこで止めて `transaction_rollback`、全部成功したら `transaction_commit`(戻すので atomic のときは `stopOnError` に関係なく最初の失敗で止まる)。play / stop / open_scene / new_scene / open_project と undo / redo / transaction_* はトランザクションの中で使えないので、atomic を省略していれば自動で外し(`transaction.note` に理由)、`atomic:true` を明示していればエラー。呼ぶ側が既にトランザクションを開いていたらその中で実行する(閉じるのは呼んだ側)。`atomic:false` は従来どおり 1 つずつ確定し、`stopOnError=true` なら最初の失敗以降を skip 記録。往復削減用。
**`dx12_focus_and_screenshot` 実装**: `focus_camera` → (1フレーム描画) → `screenshot` → 画像読み込み → 画像コンテンツ返却。
**`dx12_scatter` 実装**: seed 付き乱数(mulberry32)で位置を決め、`create_entity`/`spawn_model`/`spawn_prefab` を1体ずつ実行(+必要なら `set_transform`/`snap_to_ground`)。同じ seed なら同じ配置になる(リトライで再現)。失敗3件で打ち切り。
**`dx12_screenshot_from` 実装**: `set_editor_camera` → (1フレーム描画) → `screenshot`。
**`dx12_preview_model` 実装**: `spawn_model`(y=-10000 の遠方) → `focus_camera` → `screenshot` → `delete_entity`。失敗時も一時エンティティは削除する。

### 4-6. 実行・入力シミュレーション・計測

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_key_down` / `dx12_key_up` | `{key:int(VK) \| string("W","SPACE","UP","F1"…)}` | `{key}` ※押しっぱなしの挙動確認。Lua の `input:isKeyDown` / `keyDown()` に効く（`GetAsyncKeyState` を直接読む経路には効かない）。ウィンドウがフォーカスを失うと合成キーはクリアされる |
| `dx12_key_up` | `{key:int(VK) \| string}` | `{key}` ※`dx12_key_down` で押したキーを離す |
| `dx12_key_press` | `{key}` | `{key}` ※1 フレームだけ押して離す（`isKeyPressed` / `keyPressed()` が 1 回立つ）|
| `dx12_mouse_move` | `{dx?:f, dy?:f}` | `{dx, dy, mode, note}` ※合成マウス移動を次の 1 フレームぶんだけ注入する。一人称の視点操作はこれが唯一の口(★`camera:setYaw()` では向きを変えられない。エンジン標準の FpsController が yaw を Lua のローカル変数で持ち毎フレーム上書きするため)。押しっぱなしの概念は無いので、回し続けるには `dx12_step_frames` と交互に撃つこと。目標角度へ向けたいなら `dx12_play_script` の yaw や `dx12_autoplay` を使う方が確実(実測して比例で詰める閉ループになっている) |
| `dx12_step_frames` | `{frames?:int=1(1..600)}` | `{frames}` ※**N フレーム進んでから応答する同期バリア**。入力がシミュレーションに効いてから観測するために挟む。※決定論ステッパではない（各フレームの dt は実時間）|
| `dx12_lua_step` | `{before?:lua, frames?:int=1(1..600), after?:lua, every?:int, keys?:[key], deterministic?, dt?, hold?}` | `{frames, before?, after? | samples:[{frame,result}], step}` ※**`eval_lua` → `step_frames` → `eval_lua` を 1 回に束ねた合成ツール**（往復ごとにターンを使わない）。`keys` は進めている間押し続け、失敗しても必ず離す。`every` で途中も読む（最大 60 回）。Lua の失敗は `stage`（before / after(frame N)）付き。任意の Lua を走らせるので **guarded**（`eval_lua` と同じ扱い） |
| `dx12_perf_stats` | `{window?:int=60(..240)}` | `fps` / `frameMs{avg,min,max,p95}` / `cpu{workMs,fenceWaitMs,presentMs}` / `gpuPassMs{total,shadows,depthPrepass,prepassSsao,clusterCull,raytracing,rtScreen,ddgi,screenSpaceGi,volFog,hiZ,mainScene,particles,postFx,ui,vgCull}` / `drawCalls` / `culled` / `triangles` / `occlusion{...}` / `analysis{verdict:"gpu-bound"\|"cpu-bound"\|"fps-limit-capped"…, notes}` ※**FPS が出ないときはまずこれで犯人を特定する** |
| `dx12_benchmark` | `{...}` | 規模の梯子を測るベンチハーネス（同一シーンを条件を変えて回し、どこで折れるかを出す）|

**入力テストの型**:

```
dx12_play
dx12_key_down(key:"D") → dx12_step_frames(frames:30) → dx12_get_entity(name:"Player")   # 右へ動いたか
                        → dx12_project_world_to_screen(name:"Player")                    # 画面内に居るか
dx12_key_up(key:"D") → dx12_get_script_errors()    # Lua が死んでいないか
dx12_stop
```

★合成入力より**人間に遊ばせて `dx12_get_play_session` を読む方が正確**（Play を押した時点で記録は始まっている）。

---

### 4-7. Git / GitHub（同期）

エディタの「Git」窓と同じ操作。対象は**プロジェクトフォルダの git リポジトリ**で、
エンジンの状態は変えない。認証はユーザーの既存の git/gh 資格情報に委ねる。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_git_status` | `{}` | `{branch, remoteUrl, changedFiles[{path,status,staged}], mergeInProgress, conflicts[], githubUser}` ※status は `A`追加/未追跡 `M`変更 `D`削除 `R`リネーム `U`競合 |
| `dx12_git_branches` | `{}` | `{current, branches[{name,current}]}` |
| `dx12_git_checkout` | `{name:string, create?:bool=false}` | `{succeeded, exitCode, output, branch, created}` |
| `dx12_git_merge` | `{name:string, noFastForward?:bool=true}` | `{succeeded, ..., mergedFrom, into, mergeInProgress, conflicts[]}` |
| `dx12_git_merge_abort` | `{}` | `{succeeded, exitCode, output}` |
| `dx12_git_commit` | `{message:string}` | `{succeeded, ..., branch}` |
| `dx12_git_push` | `{}` | `{succeeded, ..., branch}` |
| `dx12_git_pull` | `{}` | `{succeeded, ..., mergeInProgress, conflicts[]}` |
| `dx12_git_fetch` | `{}` | `{succeeded, exitCode, output}` |

**約束事**:

- **まず `dx12_git_status` を読む。** 未コミットの変更があると `dx12_git_merge` は実行前に弾かれるし、
  `mergeInProgress:true` の間にできるのは「マージを終わらせるコミット」だけ。
- ブランチ名は `dx12_git_branches` から取る（推測しない）。
- **`dx12_git_checkout` / `dx12_git_pull` はプロジェクトの assets を入れ替える。**
  シーンや `.hlsl` が消える/増えるので、実行後は `dx12_list_scenes` と `dx12_git_status` を読み直す。
  ★エディタで開いているシーンのファイルが消えたら、そのまま編集を続けないこと。
- コンフリクトは `succeeded:false` + `conflicts[]` で返る（エラーではない＝作業ツリーが止まった状態）。
  直して `dx12_git_commit` で完了させるか、`dx12_git_merge_abort` で取り消す。
- **`dx12_git_push` は外向きの操作**（他人から見える場所へ出る）。ユーザーが明示的に頼んでいないなら撃たない。

```
dx12_git_status                                  # 汚れていないか / マージ中でないか
dx12_git_branches                                # 取り込み元の正確な名前
dx12_git_merge(name:"feature/x")                 # conflicts[] が空なら完了
  └ 競合したら → 各ファイルを直す → dx12_git_commit(message:"...")
  └ 諦めるなら → dx12_git_merge_abort
```

---

### 4-8. VFX(パーティクル / トレイル)

エンジンのパーティクルは 1 レイヤー 40 フィールド近くあり、生の数値を並べても「それらしく」ならない。
①`dx12_vfx_library`(何が作れるか)→②`dx12_vfx_apply`(レシピ+倍率で複数レイヤーまとめて置く)→
③`dx12_vfx_preview`(時間を進めながら連写して本当に出ているか確認)の順に使う。下 3 本の生レイヤー操作は
レシピから外れた微調整用。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_list_particle_layers` | `{entity?/name?}` | `{layers:[{index, name, kind, rate, looping, offset, vfxPath}], count}` ※放出器のレイヤー一覧。`dx12_set_component` の `layer` 引数(添字/名前)や Trigger の PlayEffect/StopEffect に渡すレイヤー名はここで分かる |
| `dx12_add_particle_layer` | `{entity?/name?, layerName?:string}` | 追加後のレイヤー情報 ※放出器にレイヤーを 1 枚足す(上限 16 枚)。★これが無いと 1 枚目しか触れない=「炎に煙を重ねる」ができない。レシピから一気に組むなら `dx12_vfx_apply` の方が速い(内部でこれを呼ぶ) |
| `dx12_remove_particle_layer` | `{entity?/name?, layer:int\|string}` | 削除結果 ※放出器のレイヤーを 1 枚消す。★最後の 1 枚は消せない。放出器ごと消すなら `dx12_remove_component(component:'particleEmitter')` |
| `dx12_vfx_library` | `{tag?:string, id?:string}` | `id` 省略時 `{presets:[...], count, tags}`、`id` 指定時はそのレシピの全レイヤー実値と注意書き ※松明/焚き火/爆発/魔法陣/雨/雪/剣閃など。★どれも複数レイヤーの重ね合わせで作ってある(炎+煙+火の粉) |
| `dx12_vfx_apply` | `{preset:string, entity?/name?, position?:[x,y,z], entityName?:string, parentName?:string, scale?:f, rate?:f, intensity?:f, color?:[r,g,b], oneShot?:bool, duration?:f, life?:f, light?:bool, replaceLayers?:bool, dryRun?:bool}` | `{applied, preset, title, entityId, name, created, layersApplied[], layersRemoved, trailApplied, current, estimatedLiveParticles, notes, lookHint, warnings, next}` ※レシピから複数レイヤーの放出器を 1 コールで組み立てる(新規作成 or 既存へ付与)。★Editor 限定(新規生成を伴うため)。置いた後は必ず `dx12_vfx_preview` で確認すること |
| `dx12_vfx_preview` | `{entity?/name?, seconds?:f=1.5, frames?:int=6(2..12), distance?:f=3, height?:f=0.5, fire?:bool, columns?:int=3, additive?:bool=true, baseline?:bool=true}` | PNG 画像ブロック + text(計測値と助言) ※放出器に寄って時間を進めながら連写し、格子画像+計測値を返す。★静止画 1 枚では「たまたま写っていない」のか「そもそも出ていない」のか区別できない。ワンショットは `fire:true` で試し撃ちしてから撮る |

### 4-9. ルック(絵作り)

ポストは約 90 フィールドあり、1 つずつ触っても bloom と exposure だけ上げて終わりがち。
光+空気+グレーディングを組み合わせで渡す。`dx12_apply_lighting_preset`(太陽+ごく一部のポスト)は土台、
ここは土台の上に乗せる仕上げ(フォグ・トーンマッパー・ビネット・粒子・色収差まで)。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_look_library` | `{tag?:string, id?:string}` | `id` 省略時 `{looks:[{id,title,summary,tags,touches,pairsWith}], count, tags}`、`id` 指定時は全フィールドの実値と注意書き ※ゴールデンアワー/ネオンノワール/ホラー/白黒/水中など |
| `dx12_look_apply` | `{preset:string, strength?:f=1, parts?:("sun"\|"fog"\|"post"\|"sky")[], dryRun?:bool}` | `{applied, preset, title, parts, strength, sun, fog, sky, postRequested, currentPost, mismatched?, hint?, notes, pairsWith, warnings, next}` ※太陽+ボリュメトリックフォグ+背景の強さ+ポスト約 20 項目を 1 コールでまとめて当てる(冪等)。★`strength` はポストにだけ効く(0 で無味無臭の値に寄る)。`parts:['post']` で今の光を壊さずグレーディングだけ乗せる。★暗いルックは光源を置いてから当てること(真っ暗なシーンに当てても真っ黒になるだけ) |

### 4-10. デカール(投影テクスチャ: 弾痕・焦げ・血・水たまり・苔・汚れ)

「そこで何かが起きた」を語る唯一の安い手段。1 つも無い床はどれだけ光を凝ってもショールームに見える。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_decal_library` | `{}` | `{decals:[{id,title,summary,defaultSize,surface,changes,notes}], count, atlas, next}` ※貼れる汚れ・傷の一覧。★水たまり/血だまり/油/雪はほぼ水平面専用(角度フェードが小さい)。壁には dirt / leak / blood_splatter を使う |
| `dx12_decal_apply` | `{preset:string, position:[x,y,z], normal?:[x,y,z](既定[0,1,0]), size?:f, depth?:f, rotationDeg?:f, opacity?:f, tint?:[r,g,b], sortOrder?:int, count?:int(1..24), spread?:f, seed?:int, entityName?:string, parentName?:string, dryRun?:bool}` | 貼ったデカールの姿勢と値 ※面へ投影して DecalComponent 付きエンティティを作る。★初回はアトラス画像(`assets/textures/decals/atlas.png`)を手続き生成してシーンに設定する(無いと無言で何も出ない)。位置と法線は `dx12_raycast_precise`/`dx12_pick` の `worldPos`/`worldNormal` をそのまま渡すのが正確。★Editor 限定。`count>1` で散らして複数枚貼れる |

### 4-11. 演出(カットシーン / シーケンス)

カメラ・ポスト・時間・エフェクト・音が同じ時間軸で噛み合って初めて演出になる。宣言的な台本→生成コードに固定し、
時間の扱いと後始末を 1 箇所で正しくする(AI に Lua を直接書かせると毎回ちがう自己流の状態機械が生える)。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_sequence_author` | `{name:string, tracks:object[], camera?:string, loop?:bool, attachTo?:string, doneEvent?:string, activateCamera?:bool, dryRun?:bool}` | `{applied, name, path, camera, attachedTo, createdEntity, duration, trackCount, playEvent, stopEvent, doneEvent, referenced, vfx, warnings, next}` ※時間軸の台本(JSON)から Lua コンポーネントを生成して貼る。track の type: camera(位置移動+注視) / fade / post / timeScale(スローモ) / shake / vfx / sound / move・rotate / light / event / scene / log。★時計は実時間(タイムスケール非適用)で進むので、スローモを掛けても台本は実時間で流れる。終了時にタイムスケールを 1.0 へ戻す。★カメラを動かすには `camera` に CameraComponent 持ちのエンティティ名が要る(グローバルカメラは毎フレーム上書きされるため)。他スクリプトから `events:emit('<name>:play'/'stop')` で操作できる |
| `dx12_sequence_preview` | `{seconds?:f=5, frames?:int=6(2..12), startDelay?:f=0, columns?:int=3, name?:string}` | PNG 画像ブロック + text(`{path, name, frames, secondsTotal, secondsPerFrame, frameDiffs, moved, recentLog, hint}`) ※Play して演出を実際に流し、ゲーム画面を時間で連写した格子画像を返す。撮影後は必ず Stop する。★撮る前に `dx12_set_editor_camera` の固定を自動解除する(残っていると演出が動いても絵が変わらない事故になる) |

### 4-11b. シーケンサー(`.dxseq`。時間軸で演出を編集 / 評価 / 再生する)

`dx12_sequence_author`(台本 → Lua 生成)とは別に、エンジンが**シーケンスそのもの**(`assets/sequences/<名前>.dxseq`)を持つ。カメラワーク・カメラカット・Transform / 任意プロパティのキー・ポスト / DoF・シェイク・イベントを 1 本の時間軸で扱い、AI は **SeqOp の JSON**(`sequence_apply_op`)で宣言的に編集する。形式・適用規則・PreAnimatedState の仕様は `docs/DXSEQ_FORMAT.md`(§18〜§20)、Lua は `Sequence.*`(`docs/API_REFERENCE.md`)。**既存の `dx12_sequence_author` / `dx12_sequence_preview` / `dx12_camera_path` は名前も挙動も返り値も残す**(移行は S6)。

エンジン method(McpMeta 直渡し。`dx12_call {name:"sequence_list"}` で再起動なしに使える。Core の `dx12_sequence {op}` が 1 本に束ねる)。時刻は **秒 `t` / ティック `tick`(既定 6000/秒)/ フレーム `frame`** のどれか(優先 tick > frame > t)。

| method(= `dx12_sequence` の op) | 効果 | params | 返り値(要点) |
|---|---|---|---|
| `sequence_list`(list) | read | なし | `{sequences:[{name,rel,loaded,dirty,durationSec,fps,bindings,tracks,cuts,…}], editor:{active,doc,tick,playing,preAnimated}, players:[…]}`(assets のファイルと開いている文書) |
| `sequence_load`(load) | runtime | `{name, create?:bool, fps?:int(30), reload?:bool}` | 文書の要約 + `bindingStatus`(バインディングごとの解決の経路 `via`(guid / path / name)・エンティティ・警告)+ `issues` + `unresolved` 数。`create:true` で無ければ空の文書を作る(ファイルは save で) |
| `sequence_save`(save) | write_file | `{name, path?}` | `{saved, rel, bytes}`。アトミック・正準形。`path` で別名保存。**dryRun:true** で書く先と大きさだけ |
| `sequence_get`(get) | read | `{name, detail?:"summary"\|"full"}` | 要約(`bindingTable` / `cutTable` / `markerTable` / `bindingStatus`)。`full` は `dxseq`(正準形の全文)と `sequence`(JSON) |
| `sequence_eval`(eval) | read | `{name, t\|tick\|frame}` | **【非破壊】** 何も書かない・guid も確定しない。`{tick, seconds, frame, cut:{index,id,camera,cameraName}, values:[{binding,track,type,path?,channel,value}], state:{<bindingId>:{position:[x,y,z], rotation, scale, "PointLight.intensity":…}}, clips, bindingStatus, unresolved}` |
| `sequence_scrub`(scrub) | runtime(Editor 限定) | `{name, t\|tick\|frame}` または `{end:true}` | エディタ上に適用。eval と同じ形 + `applied:{transformsWritten,fieldsWritten,skippedPhysics,userDrift,postOverrides,shakes,warnings}` + `editor:{active,preAnimated}` + `cutCamera`。**値は PreAnimatedState が退避し、`end:true` / Play / あらゆる保存の直前に必ず元へ戻る(保存にスクラブの値は混ざらない)**。見た目は `screenshot_game_view`(カットのカメラ視点)。**dryRun:true** = 適用せず eval の結果だけ |
| `sequence_play`(play) | runtime | `{name, loop?:"once"\|"loop"\|"pingpong", rate?, from?, clock?:"real"\|"game", restoreOnEnd?, delay?}` | Play 中 = 実時間(既定。タイムスケール非適用)で再生、イベントは前進で 1 回発火。Editor 中 = プレビュー再生(値は非破壊・イベントは発火しない)。**dryRun:true** = 何が再生されるかだけ |
| `sequence_stop`(stop) | runtime | `{name?, restore?:bool(true)}` | Play 中 = 名前(省略で全部)の再生を止める(値は戻さない)/ Editor 中 = プレビューを止めて元の値へ戻す |
| `sequence_apply_op`(edit) | write_file | `{name, ops:[…], label?, undo?:bool, redo?:bool}` | SeqOp の JSON(`docs/DXSEQ_FORMAT.md` §18)。**1 回の呼び出し = Undo 1 ステップ**。途中で 1 つでも失敗したら全部巻き戻し(`error_name: E_INVALID_OP`。エラー文に `ops[i] <op>:` の位置)。ID は省略可。`addBinding` は `entity:"名前"` で対象を指せる(guid を自動確定)。`undo:true` / `redo:true` で取り消し / やり直し。**dryRun:true** で検査だけ(`{ok, wouldApply, ops}`) |
| `sequence_autoplay`(autoplay) | write_scene | `{op?:"list"\|"set"\|"add"\|"remove"\|"clear", sequence?, loop?, rate?, startDelay?, clock?, players?:[…]}` | シーンの自動再生設定(シーン JSON の `sequencePlayers`)。Play 開始時に再生される |

- **Core の `dx12_sequence {op}`**(TS。`toolset/sequenceCore.ts`)は上表の method を 1 本に束ねる。`op` = list / load / save / get / eval / scrub / play / stop / edit(= `sequence_apply_op`)/ autoplay(下位操作は `action`。エンジンへは `op` として渡す)。他の引数はそのままエンジンへ。`dryRun:true` は save / scrub / play / edit でエンジンのプレビュー、list / get / eval は読み取りなので無視して実行、load / stop / autoplay は実行せず予測を返す。エラーは M2 の封筒(`fix` は `dx12_sequence` の撃ち直し)。Core は 40 本のまま(代わりに `dx12_get_script_errors` を長尾へ。理由は `MCP_FLEET_DESIGN.md` §9)。
- **`get_entity` / `SerializeEntity` はスクラブ中でも元の値を返す**(直列化の直前フックが元へ戻すため。表示は次のフレームで再適用される)。スクラブした値の確認は `sequence_eval` か `screenshot_game_view`。
- `sequence_*` は**シーンの保存されるデータを変えない**(スクラブは保存の直前に戻す)ので、未保存フラグを立てない(`sequence_autoplay` だけは立てる)。`sequence_apply_op` / `sequence_save` が変えるのは文書(`.dxseq`)だけ。
- 時間軸の撮影: `sequence_scrub {t}` → `screenshot_game_view`(カットのカメラ視点。ポスト / DoF / シェイクも掛かる)を時刻ぶん繰り返す。Play 中の再生は `dx12_play` → `sequence_play` → `step_frames {deterministic}` → `screenshot_final`。
- 既定の時計は実時間: Lua の `time.setScale` でスローモにしても台本は止まらない。`timeScale` トラックを持つシーケンスは実時間に強制される。
- 物理: 動的な剛体 / キャラコントローラの Transform は Play 中は書かれない(警告 `skippedPhysics`)。`docs/DXSEQ_FORMAT.md` §19.2。

### 4-11c. マテリアルグラフ(`.dxmg` / `.dxmat` の `graph` キー)

ノードのグラフから HLSL を生成してフォワード描画に載せる(G2b。設計 `docs/MATERIAL_GRAPH_DESIGN.md`、仕様 `docs/MATGRAPH_FORMAT.md` / `docs/MATGRAPH_G2B.md`)。エンジン method(McpMeta 直渡し。**長尾** = core 面 40 本には入れない。`dx12_call {name:"material_graph_get"}` / `dx12_tool_search` で使う。別名 `dx12_material_graph_*`)。エディタ UI(ノード編集)は別担当で、ここは AI / スクリプト向けの入口。

| method | effect | params | 返り値(要点) |
|---|---|---|---|
| `material_graph_nodes` | read | `{query?, detail?:"summary"\|"full"}` | `{count, nodes:[{type, category, description, inputs?, outputs?, props?}]}`(置けるノード型。`edit` の `add` の型名) |
| `material_graph_get` | read | `{path(.dxmg\|.dxmat), detail?:"summary"\|"full", hlsl?, text?}` | `{name, guid, settings, nodes:{id:{type, pos, in:{pin:"id.pin"\|literal}, props}}, comments, parameters, validation:{ok, errors, warnings, hash, slots, stats, diagnostics}, instance?, engine?}`。`.dxmat` を渡すと親グラフ + params + エンジン側のビルド状況 |
| `material_graph_validate` | read | `{path, hlsl?}` | 検証 + コンパイル。診断は `{severity, code, nodeId, pin?, message, hint?}`(未接続の必須入力・型・循環・予約ピン・未対応の設定 `W_G2B_BLEND` など)。何も書かず、エンジンの状態も変えない |
| `material_graph_edit` | write_file(journal) | `{path, ops:[…], create?:bool, save?:bool}` | ops = `{op:"add",type,id?,pos?,props?,in?}` / `{op:"remove",id}` / `{op:"connect",from:"id.pin",to:"id.pin"}` / `{op:"disconnect",to:"id.pin"}` / `{op:"set",id,prop\|pin,value}`(pin は入力ピンのリテラル。null で消す)/ `{op:"move",id,pos}` / `{op:"settings",values:{…}}`。**全 op を複製へ当てて検証し、1 個でも失敗したら 1 バイトも書かない**(`error_name: E_INVALID_OP`、エラー文に `ops[i] <op>:`)。返り値 `{applied, results, changed, structureChanged, recompileRequired, validation, newHash, oldHash, wrote}`。**dryRun:true** で書かずに同じ結果 |
| `material_graph_compile` | runtime | `{path?, force?:bool, rebuild?:bool}` | 再ビルドを要求してエンジンの状況を返す: `{known, instances:[{hasActive, compiling, failed, activeHash, pendingHash, generation, valueUpdates, errorLog, diagnostics(DXC エラーは nodeId 逆引き済み), codegenMs, dxcMs, psoMs, cacheHit}], counters}`。**非ブロッキング**(HLSL が変わるビルドはワーカーで非同期、完了までは旧版で描画)。`step_frames` で進めてから `material_graph_status` で読む。未描画の材質は `known:false` |
| `material_graph_status` | read | `{path?}` | インスタンスごとの状況 + カウンタ(`dxcCompiles` `cacheHits` `psoCreates` `valueUpdates` `poolUsed` `fallbackDraws` …) |
| `material_graph_graphize` | write_file(journal) | `{path(.dxmat)}` | 従来の PBR 材質 → 標準テンプレートのインスタンス(`graph` + `params`)。**手動のみ**。元ファイルは `<name>.dxmat.bak`(最初の 1 回だけ。既にあれば守る)。返り値 `{template, params, wroteTemplate, wroteBackup, wroteInstance}`。**dryRun:true** で変換後の中身(`instanceText`)だけ |
| `material_graph_set_param` | write_file(journal) | `{path(.dxmat), name, value, save?:bool}` | params の書き換え(数値 / 配列 / テクスチャのパス / null で上書きを消す)。**再コンパイルしない**(プールのレコードを書き直すだけ)。`save:false` でメモリ上だけ。グラフに無い名前は `warnings`。**dryRun:true** 対応 |
| `material_graph_apply` | write_scene | `{entity\|name, path, submesh?:int(0)}` | エンティティのサブメッシュへ `.dxmat`(グラフ材質 / 従来材質)を割り当てる。`path:""` で解除。Undo 可 |

- 典型: `material_graph_graphize {path}` → `material_graph_apply {name, path}` → `step_frames {frames:60}` → `material_graph_status`(`hasActive:true`)→ `screenshot_final`。値の調整は `material_graph_set_param`、構造の編集は `material_graph_edit`。
- 編集した `.dxmg` はエンジンが次のフレームに読み直す(HLSL が変わらない編集は再コンパイルなし)。HLSL が変わる編集は `compiling:true` の間、旧版で描画される。失敗しても描画は止まらず、エラーは `material_graph_status` の `errorLog` / `diagnostics`(nodeId つき)に残る。
- v1 の範囲: Opaque / DefaultLit のみ(他は警告して不透明で描く)。影・深度・DXR・パストレは単色プロキシ、VG 対象外(`docs/MATGRAPH_G2B.md` §5)。

### 4-12. シーンの整理 / 命名規約

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_scene_scaffold` | `{only?:("ENV"\|"LVL"\|"LGT"\|"GP"\|"FX"\|"UI"\|"CAM")[]}` | `{groups:[{key, root, entityId, created}], convention}` ※共同開発用のグループ骨格(空エンティティ)を作る。既にあるものは作り直さない(何度撃っても安全)。★ルートは必ず原点・無回転・スケール 1(`set_parent` はワールド座標を保持しないため、単位変換でないルートにぶら下げると物がワープする) |
| `dx12_organize_scene` | `{dryRun?:bool=true, rename?:bool=true}` | `{applied, moves:[{entityId, oldName, newName, group, reparent, locked}], untouched, protectedNames, luaFilesScanned, convention}` ※既存シーンを命名規約に沿って整理(コンポーネントで分類→規約グループへ親付け→`<PREFIX>_<Kind>_<NN>` に改名)。既定 `dryRun:true`=計画のみ。★プロジェクトの `.lua` を全部読み、文字列として出てくる名前は改名しない(`scene:findEntity` は名前で引くので改名すると『エラーも出ずに OnUpdate の残りが動かない』壊れ方をする) |
| `dx12_validate_naming` | `{}` | `{pass, checked, issues:[{entityId, name, kind, text}], counts, convention}` ※命名とグループ分けの崩れ(DEFAULT_NAME/NO_PREFIX/DUPLICATE_NAME/BAD_CHARS/NOT_IN_GROUP)を数える。直すのは `dx12_organize_scene` |

### 4-13. 品質検査(アセット欠落 / 配置 / 仕上がり)

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_asset_gap` | `{includePlaceholders?:bool=true}` | `{missing:[{entityId,name,modelPath}], placeholders:[{entityId,name,primitive,sizeHint}], count, next}` ※参照切れ(modelPath があるのにファイルが無い)と、プリミティブで代用しているだけの仮置きを集める。Blender で作り始める前の入口 |
| `dx12_validate_layout` | `{fix?:"none"\|"safe"\|"all"(既定 none), tolerance?:f=0.001, judge?:bool=true}` | `{pass, checked, errors, warnings, fixed, issues:[{kind, level, entityId, name, otherEntityId?, text, fixed}], judge?:{source, findings:[{code, ref, entityId, name, intended, keep}], uncertain[{id, why, look}], errorsExcludingKept, warningsExcludingKept, passExcludingKept, notAsked[], skipped, cost}}` ※埋まり(BURIED)/浮き(FLOATING)/ちらつき(Z_FIGHT)/深いめり込み(OVERLAP)/二重配置(DUPLICATE)/当たり判定欠落(NO_COLLIDER・COLLIDER_WITHOUT_BODY)/スケール異常(SCALE_ANOMALY・NAN_TRANSFORM)を数値で拾う。Editor 限定(Playing 中は MODE_CONFLICT)。★`COLLIDER_WITHOUT_BODY` はこのエンジン固有の罠(boxCollider だけでは Jolt に載らず床をすり抜ける)。`fix:'safe'` で BURIED/FLOATING・Z_FIGHT・COLLIDER_WITHOUT_BODY を自動修正。DUPLICATE は取り返しがつかないので報告のみ。★`judge` は判断段(§4-16): OVERLAP / FLOATING / BURIED / NO_COLLIDER だけを名前・グループ・大きさ・程度の言葉と Brief から「意図的か」聞く(本棚の中の本・吊りランプ・半分埋めた岩・すり抜けてよい草は keep:true)。Z_FIGHT / DUPLICATE / COLLIDER_WITHOUT_BODY / NAN_TRANSFORM / SCALE_ANOMALY は明らかな欠陥なので聞かない |
| `dx12_polish_audit` | `{screenshot?:bool=true, only?:("light"\|"air"\|"grade"\|"motion"\|"material"\|"contact"\|"image")[], sampleMeshes?:int=24, judge?:bool=true}` | `{score, verdict, findings:[{code, category, severity, what, why, fix}], facts, judge?:{source, briefFit, findings:[{code, intended, keep}], nextFix:{id, tool, args, confidence}, uncertain[], scoreExcludingKept, briefMissing?}}` ※高品質な絵に必ず入っている要素が揃っているかを測り、足りないものを効く順(光→空気→階調→動き→素材→接地)で返す。各指摘に「なぜ安っぽく見えるか」と「次に撃つコマンド」が付く。★`dx12_diagnose` は壊れているか、`dx12_look_compare` は参照画像との差を見る道具で、これは参照画像なしに「作りかけに見える理由」を言うためのもの。★`judge` は判断段(§4-16): 同じ指摘を作品の意図(Brief)に照らして仕分け、意図どおりのもの(`keep:true`)は直さない。`judge:false` で止まる |
| `dx12_perceive` | `{camera?:"editor"\|"game"\|{position:[x,y,z], target:[x,y,z], fovDeg?:f}, targets?:string\|string[], top?:int=8, width?:int, height?:int, settleFrames?:int=8, path?:string, includeTransparent?:bool=true}` | `{facts:{viewpoint, scene:{brightness, upper_half, lower_half, left_half, right_half, sky_or_void, black_crush, blown_out, farthest_surface, dominant}, targets:[{name, facts:{visibility, screen_share, position, brightness, contrast, texture, lit_side, main_light?, occluded, fits_in_view, distance, saturation, material?}}], top[]}, raw}` ※知覚層。指定の視点から見た画面を ID パスで集計し、対象が【プレイヤーの目にどう見えるか】を数値(raw)と数値を含まない言葉(facts。言葉の境界は `perceive.ts` の `PERCEIVE_BINS`)で返す(遅延応答。普段 0.1〜0.4 秒)。遮蔽率は targets で名指しした対象だけ。スプライト/パーティクル/UI は数えない。★`lit_side`(litFacing)はカスタムシェーダの照明を見ていないので、「照らされている」でも brightness / contrast が暗ければ暗い(必ず一緒に読む)。品質ゲートの読みやすさの検査(`readability`)が使う |
| `dx12_quality_gate` | `{checks?:("scene"\|"layout"\|"polish"\|"ui"\|"readability"\|"playtests")[], heavy?:bool=false, screenshot?:bool=true, strictness?:"balanced"\|"strict", screen?:string, playtests?:bool\|string[], readability?:[{label?, camera?:"editor"\|"game"\|{position, target, fovDeg?}, targets:(string\|{name, role?})[]}](最大 4), judge?:bool=true, bundle?:"perDomain"\|"one"(既定 perDomain)}` | `{pass, blocking[], keep[{…item, judge:{question, value, threshold, confidence, source}, why}], suggestions[{check, text, tool?, args?}], uncertain[{check, id, why, look:{tool, args}}], counts:{blocking, keep, uncertain, suggestions}, blockingByCode:{"検査:コード":件数}, truncated, cost:{requests, tokens, usd, ms}, checks[{id, title, ran, skipped?, ms, summary, judge?}], judge:{used, source, briefMissing?, bundle, bundles}, elapsedMs, next}` ※作業の区切りで 1 回撃つ品質ゲート(§4-17)。シーンの検証・配置・仕上がり・UI・(指定すれば)読みやすさとプレイテストのルールの結論を 1 つの合否にまとめ、Jev が Brief に照らして意図どおり(keep)と判断したものは blocking から外す。uncertain は合否に入れず、見るためのツール呼び出し付きで返す |

### 4-14. Blender 連携(自動起動 → PBR素材/仕上げ → 規約どおり書き出し → 実寸検証)

アドオンの既定は全部 OFF なので、放っておくと AI はプリミティブだけで真っ白でのっぺりしたモデルを組む。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_model_brief` | `{kind?:string(character/prop/level/家具等)}` | `{rules, materials, gotchas}` ※dx12 へ持ってくるモデルの作り方(Blender で作り始める前に読む)。★単色マテリアル禁止(glTF の baseColorFactor を読まないので真っ白になる)/アルファ抜きが無いので葉・枝カードは Blender で消してから出す/単位はメートル・原点は底面中心・+Y が正面/テクセル密度 512〜1024texel/m/ORM は G=roughness B=metallic/シェイプキーは捨てる |
| `dx12_blender_ensure` | `{blenderPath?:string, timeoutMs?:f=60000}` | `{running, started, port, blenderPath?, waitedMs?, ...assetSources}` ※BlenderMCP アドオンのソケット(127.0.0.1:9876)が生きているか確かめ、死んでいたら Blender を起動してポートが開くまで待つ。PolyHaven も自動で有効化する。★モデリングを頼まれたらまずこれを撃つ(手で Blender を開いてもらう必要は無い) |
| `dx12_blender_polish` | `{objects?:string[], bevelWidth?:f=0.003, bevelSegments?:int=2, smoothAngle?:f=30, uvMeters?:f=1.0, minThickness?:f=0.004}` | Blender 実行結果(処理内容の要約) ※「うすぺらい」を消す一括処理: ①スケール適用(★ベベルより先。非一様スケールのままだと角が歪む) ②厚みゼロの板に Solidify ③UV を実寸で切り直す ④スムーズ+自動スムーズ ⑤ベベル ⑥加重法線。書き出す前に必ず通すこと |
| `dx12_blender_material` | `{objects?:string[], assetId?:string, keyword?:string, resolution?:"1k"\|"2k"\|"4k"(既定2k), uvMeters?:f=2.0}` | Blender 実行結果(貼った素材の情報) ※PolyHaven(CC0・API キー不要)から PBR 素材を落として貼る。★エンジンは glTF の baseColorFactor を読まないのでテクスチャ無しのマテリアルは真っ白になる。ORM は arm マップがあればそのまま、無ければ Rough から B=0 で合成(単体のまま出すと木や布が金属として描かれる) |
| `dx12_blender_export` | `{objects?:string[], destPath:string, clearShapeKeys?:bool=true, applyModifiers?:bool=true}` | `{destPath, absPath, exported[], bytes, renamedImages[], warnings[], assetInfo, next}` ※Blender の選択物を dx12 の規約どおり glTF で書き出し assets へ取り込み実寸まで検証する。★シェイプキーを捨てる/画像テクスチャが 1 枚も無いマテリアルを警告/`tmpXXXX.jpg` 名の画像を意味のある名前へ直し uri も書き換える。取り込み後 `dx12_asset_info` で実寸を読むので cm/m の取り違えもその場で分かる |
| `dx12_scene_env` | `{assetId?:string, keyword?:string, resolution?:"1k"\|"2k"\|"4k"(既定2k), iblIntensity?:f=1.0, skyboxIntensity?:f=0.35, drawSkybox?:bool=true}` | `{assetId, path, bytes, skybox, note}` ※PolyHaven の HDRI(CC0・API キー不要)を落としてシーンの環境マップにする。★既定の手続き空のままだと全部に青が乗って彩度が落ちる。金属と光沢は環境に映るものが無いと質感が出ない。★屋内シーンで環境光を効かせたくない場合は使わないこと(envMapPath があると DirectionalLight.ambient が無視される) |

### 4-15. テストプレイ(決定論台本 / 移動能力の実測 / 到達性 / 回帰テスト)

コードは ctest で守られているのに遊びは誰も守っていない、という穴を埋めるための一群。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_play_script` | `{steps:[{t:f, down?:string\|string[], up?:string\|string[], press?:string\|string[], yaw?:f, note?:string}], expect?:[{at?:f, by?:f, near?:[x,y,z], radius?:f, yAbove?:f, yBelow?:f, grounded?:bool, movedAtLeast?:f, label?:string}], player?:string, until?:f, sampleHz?:f, dt?:f, autoPlay?:bool=true}` | `{pass, player, durationSec, dt, results[], trace[], samples, mouseDegPerPixel?, yawWarnings?, next?}` ※入力タイムラインと合否条件を 1 コールで走らせる(dt を 1/60 に固定=同じ台本なら毎回同じ結果になる)。yaw(度) はマウス移動注入の閉ループで合わせる(★`camera:setYaw()` では向けられない)。落ちた条件は「どれだけ足りなかったか」を返す |
| `dx12_measure_player` | `{player?:string, forwardKey?:string=W, jumpKey?:string=SPACE, save?:bool=true}` | `{walkSpeed, jumpHeight, jumpDistance, stepHeight, maxSlopeDeg, measuredAt, warnings, savedTo?, startPos}` ※プレイヤーを実際に歩かせ・跳ばせて【歩行速度/ジャンプ高/ジャンプ距離】を実測する(宣言値ではなく Lua の実装値)。結果は `<project>/.dx12/movement.json` に保存され、`dx12_check_reachable` の判定根拠になる |
| `dx12_check_reachable` | `{from?:[x,y,z], fromName?:string, to?:[x,y,z], toName?:string}` | `{reachable, warnings, capability, start?, goal?, pathPoints?, pathLength?, issues?, estimatedWalkSec?, reason?, next?}` ※ナビメッシュの経路と実測した移動能力で「そこへ行けるか」を判定する(Play しないので何度でも撃てる)。★先に `dx12_measure_player` を撃つこと(実測値が無ければ保存値→保守的な既定値の順にフォールバックし warning を出す) |
| `dx12_autoplay` | `{goal?:[x,y,z], goalName?:string, player?:string, forwardKey?:string=W, jumpKey?:string=SPACE, arriveRadius?:f=1.5, timeoutSec?:f=60, judge?:bool=true}` | `{cleared, player, goal, finalPos?, notes, remainingDistance?, waypointsReached?, waypoints?, trace, elapsedSec, stuckAt?, stuckAtWaypoint?, reason?, next?, judge?}` ※ナビメッシュの経路を実際の入力(マウス+WASD+ジャンプ)でなぞって本当にゴールへ行けるか確かめる。★`dx12_check_reachable`(静的判定)の実証版。詰まったら座標付きで返す。★届かなかったときだけ `judge`(原因だけ。機械は迷わないので困り度は聞かない) |
| `dx12_record_playtest` | `{name:string, endTolerance?:f=1.0, pathTolerance?:f=2.0, note?:string, goalName?:string, judge?:bool=true}` | `{saved, name, scene, durationSec, inputs, samples, humanDrift, warnings, next, judge?}` ※直前の 1 プレイ(`dx12_get_play_session`)を `.playtest` として保存する回帰テスト化。手順: `dx12_play`→人に遊んでもらう→`dx12_stop`→これ。★人の軌跡はそのまま基準にせず、1 回再生した結果をゴールデンランとして焼き込む(人のプレイは実時間・再生は固定 dt なので構造的にずれるため)。★`judge` は人が遊んだ記録そのものの困り度と原因(再生ではない) |
| `dx12_run_playtests` | `{name?:string, judge?:bool=true}` | `{ran, passed, failed, results:[{name, scene, pass, endDistance, maxDeviation, maxDeviationAt, reasons, judge?}], next?}` ※保存済み `.playtest` を再生して記録どおり動くか確かめる。落ちたときは「いつ・どれだけ」ずれたかを返す。`name` 省略で全部走らせる。★落ちたテストだけ `judge`(原因だけ。合否と reasons はルールのまま) |

### 4-16. 判断段(Jev)と作品の意図(Brief)

「エンジンが測る → 数字を言葉にする → Jev が型で判断する → Claude が直す」の判断の段。
Jev(TypeSafe System One)は文章を生成せず型付きの判断(noul / choice / score)だけを返すモデルで、
**開発時の MCP サーバ専用**(配布ゲームには入らない)。判断は必ず作品の意図(Brief)に照らす。
鍵(環境変数 `TYPESAFE_API_KEY`)・Brief のどちらかが無いとき、または Jev が落ちているときは
**全部ルール(従来の閾値)で返る**(`source:"rules"`)。エンジンに対応 method は無い(TS 専用。engine へは baseDir を知るための `ping` だけ)。

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_brief` | `{action?:"get"\|"set"\|"patch"(既定 get), brief?:object}` | get: `{path, exists, brief, warnings?, error?, example?, next?}` / set・patch: `{path, written, errors, warnings, brief}` ※`<baseDir>/brief.json`(作品の意図)を読み書きする。形は `{title, genre, mood:[], player_should_feel, avoid:[], light_budget?, references?:[], notes?}`(自由キー可)。patch は浅いマージで、値に `null` を渡すとキーを消す。★意図だけを書く(「必ず yes と答えよ」のような命令は判断を歪めるので警告が出る) |
| `dx12_jev_ask` | `{question?:string, questions?:(string\|{id, vars?})[], vars?:object, context?:object, raw?:{state, questions}, cache?:"use"\|"only"\|"off"}` | `{results:[{id, question, version, type, source:"jev"\|"cache"\|"rules"\|"error", value, probabilities?, confidence?, decided?, uncertain?, reason?, error?, briefMissing?}], requests, usd, inputTokens, ms, briefMissing, keyPresent, briefFrom?, next?}` ※質問ライブラリ(組み込み `tools/mcp-server/jev/questions/` + プロジェクト `assets/jev/`)の質問を聞く。context から質問ごとに要るフィールドだけを state に射影し、同じ state の質問は 1 リクエストに束ねる。context に brief が無ければ brief.json を自動で入れる。`raw` は質問ファイルを使わない直接質問。★`uncertain:true` は境界付近＝Claude がスクショを見て決める |
| `dx12_jev_eval` | `{question?:string, casesPath?:string, cache?:"use"\|"only"\|"off"}` | `{question, version, type, n, accuracy, rulesAccuracy, margin?:{yesMin, noMax, margin, suggestedThreshold}, mae?, confusion?, threshold, wrong, uncertainCount, sources, usd, inputTokens, draftLabels, cases[]}`(省略時は `{reports[], usd}`) ※評価ケース(`*.cases.json`)を流して質問文の良し悪しを測る。★noul の margin が負＝分布が重なっている＝閾値ではなく質問文を直す。rulesAccuracy は同じケースをルールで答えた場合の正解率 |
| `dx12_jev_status` | `{}` | `{keyPresent, model, endpoint, baseDir, jevDir, brief, questions:[{id, version, type, origin, cases, file}], questionErrors, log:{requests, errors, cacheHits, inputTokens, outputTokens, usd}, cacheEntries}` ※鍵の有無(値は出さない)・質問一覧・`<baseDir>/.dx12/jev/log.jsonl` からの累計費用・キャッシュ件数 |

**仕組み**

- 流れは「エンジンが測る → 数値を言葉にする(`jev/wordify.ts`) → Jev が型で判断する → Claude が直す」。
  Jev は数値の大小・近さに弱い(公式 model-jaggedness)ので、比較は TS で済ませて **state には言葉だけ**を入れる
  (「平均輝度 0.07」ではなく「とても暗い」)。元の数値は結果の `raw` / `facts` に残す。ビンの境界は `BINS` の 1 か所で、
  polish の閾値(眠い 0.35 / 白飛び 8% / 真っ黒 35% / 彩度 0.08)と揃えてある。
- 質問は `tools/mcp-server/jev/questions/*.jevq.json`: `{id, version, type, instructions, criteria?, state:["brief","facts.look",…],
  threshold?:{yes?, pass?, minConfidence?, band?}, fallback?, cases?, notes?, lookup?}`。質問文は英語、state は日本語でよい。
  同じ id をプロジェクトの `assets/jev/` に置くとそちらが勝つ。
- **state は質問ごとに要るフィールドだけへ射影し、射影が同じ質問は 1 リクエストに束ねる**(並列評価なので遅延は増えない)。
  `dx12_polish_audit` の 3 種(`look.brief_fit` / `look.next_fix` / 指摘ごとの `finding.intended`)は state が共通なので 1 往復。
- キャッシュ: `<baseDir>/.dx12/jev/cache/`(キー = 質問 id + 版 + モデル + 質問本文 + 正規化 state)。
  `cache:"only"` はネットに出ない(無ければルール)。記録: `<baseDir>/.dx12/jev/log.jsonl` に 1 行 1 リクエスト(state 本文は書かない)。
- 閾値は評価ケースで決める: noul の `yes` は margin の境界の中点(肯定側が甘いので 0.5 にしない)。score は段の番号より合否(`pass`)を使う
  (強く合う絵を 3.2〜3.4 と控えめに付ける癖がある)。choice は `minConfidence` 未満を uncertain にする。
  根拠(日付・件数・margin・直した回数)は各質問ファイルの `notes`。実 API での再測定は `node tools/mcp-server/jev/runEval.ts --cache off`。
- 2026-09-25 時点の実測(ラベルは claude-draft): `finding.intended` 正解率 1.0・margin +0.13〜+0.19(ルールだけなら 0.52) /
  `look.brief_fit` 合否 16/16・合否 margin 0.95〜1.13 / `look.next_fix` 正解率 0.79〜0.84(ルールだけなら 0.21)、外れは全部 uncertain。

**judge の共通の形**(`jev/judgeCommon.ts`): どの判断段も `{source:"jev"|"cache"|"rules", briefMissing?, reason?, findings:[{code, intended, keep, uncertain?, ref?, entityId?, name?}], uncertain:[{id, why, look?:{tool, args}}], cost:{requests, tokens, usd, ms}}` を持ち、ツール固有のもの(`briefFit` / `nextFix` / `confusion` / `cause` / `passExcludingKept` …)をその外に足す。`intended` は noul の yes の確率(ルールのときは null)、`keep:true` は Brief に照らして意図どおり＝直さない。`uncertain` の `look` はそのまま撃てる「見るためのツール呼び出し」。どのツールも `judge:false` で判断段を止められ、既存のフィールドは一切変えない。

| ツール | 聞くもの(質問 id) | 聞かないもの(ルールのまま) | 固有の judge フィールド |
|---|---|---|---|
| `dx12_polish_audit` | 絵の Brief 適合(`look.brief_fit`)・次の一手(`look.next_fix`)・指摘ごとの意図(`finding.intended`) | — | `briefFit` / `nextFix` / `scoreExcludingKept` |
| `dx12_ui_audit` | UI の Brief 適合(`ui.brief_fit`)・好みの指摘 7 種の意図(`ui.finding_intended`) | 押せない・読めない・崩れている 12 種(`notAsked`) | `briefFit` / `passExcludingKept` / `scoreExcludingKept` |
| `dx12_validate_layout` | OVERLAP / FLOATING / BURIED(`layout.intended`)、NO_COLLIDER(`layout.no_collider_ok`) | Z_FIGHT / DUPLICATE / COLLIDER_WITHOUT_BODY / NAN_TRANSFORM / SCALE_ANOMALY | `errorsExcludingKept` / `passExcludingKept` / `skipped` |
| `dx12_get_play_session` / `dx12_record_playtest` | 困り度(`play.confusion`)・原因(`play.cause`) | 到達・再生の合否 | `confusion` / `cause` / `words` |
| `dx12_autoplay` / `dx12_run_playtests` | 原因(`play.cause`)だけ | 困り度(機械は迷わない)・合否 | `cause` / `words` |
| `dx12_quality_gate` の readability | 初見で気づけるか(`read.noticeable`)・主な原因(`read.main_problem`) | — | `targets[{noticeable, mainProblem}]` / `unreadable` |

- 事実はすべて言葉にしてから渡す: UI は要素数・画面の文言(数字は `#`)・偏り・中央揃えの割合・フォントの種類・面色の系統・装飾の数、配置は名前(連番を落とす)・グループ・親・大きさ・程度(指摘文から読む。書式は C++ の snprintf と突き合わせてテスト)、プレイは区間(最大 4)ごとの止まり・押しても動かない・行き来・落下・戻された(落下の後か)・立ち止まっての見回し・その場ジャンプ・ゴールへの進み、読みやすさは `perceive.ts` の facts(★`lit_side` はカスタムシェーダを見ないので必ず brightness と並べ、質問文でも暗さを優先させる)。
- 質問は 1 往復: 同じ判断段の質問は state が共通(`brief + facts.<ui|layout|play|read>`)で 1 リクエストに束なる。配置と読みやすさは対象を英字の ref(A, B, …)で名指しする(Jev は数を数えられない)。
- 2026-09-25 の実測(各 3 回以上、`node jev/runEval.ts --cache off`、ラベルは claude-draft。閾値は全回を合わせた境界の中点):

| 質問 | 型 | 件数 | 正解率 | margin(境界) | 閾値 | ルールだけ | 直した回数 |
|---|---|---|---|---|---|---|---|
| `ui.finding_intended` | noul | 18 | 1.0 | +0.52(0.76 / 0.24) | yes 0.50 | 0.556 | 1(v2) |
| `ui.brief_fit` | score | 14 | 合否 1.0(段 0.571) | 合否 0.78(3.03 / 2.25) | pass 2.64 | — | 0 |
| `layout.intended` | noul | 15 | 1.0 | +0.28(0.72 / 0.44) | yes 0.58 | 0.467 | 1(v2) |
| `layout.no_collider_ok` | noul | 12 | 1.0 | +0.16(0.76 / 0.60) | yes 0.68 | 0.833 | 1(v2) |
| `play.confusion` | score | 13 | 合否 1.0(段 0.46) | 合否 0.21(1.19 / 0.98) | 困っている線 1.09 | 段 0.385 | 1(v2) |
| `play.cause` | choice | 16 | 0.94〜1.0 | — | minConfidence 0.5 | 0.688 | 2(v3) |
| `read.noticeable` | noul | 15 | 1.0 | +0.24(0.71 / 0.47) | yes 0.59 | 0.933 | 3(v4) |
| `read.main_problem` | choice | 15 | 0.933(誤りは全部 uncertain) | — | minConfidence 0.5 | 0.8 | 2(v3) |

  直した経緯(何が外れ、どの語が衝突したか)は各質問ファイルの `notes`。★Jev の答えを見てから直した / 広げたラベルが 2 件ある(`layout.no_collider_ok` の写実の森のホログラム看板を false→true、`play.cause` の「軽快なアクションで止まって見回したがゴールに着いた人」を lost_way→lost_way か none)。人が見直すこと。

### 4-17. 品質ゲート `dx12_quality_gate`

作業の区切り(部屋を組み終えた・UI を作った・ステージを直した)で 1 回撃つ。検査の道具(validate_scene / diagnose / validate_layout / polish_audit / ui_audit / perceive / run_playtests)を 1 つの合否に畳む。本体は `tools/mcp-server/jev/qualityGate.ts`。

- **合否の規則**: ルールの error は `blocking`(直すまで先へ進まない)。Jev が keep(Brief に照らして意図どおり)と判断したものは blocking から外し、`keep[].judge`(`question` / `value` / `threshold` / `confidence`(noul は yes の確率そのもの)/ `source`)と `why` を残す。`uncertain` は合否に入れず列挙だけ(各項目に `look`)。
- **何が blocking か**: 参照切れ(validate_scene の `[ERROR]`)・diagnose の error・配置の error・UI の error(`strictness:"strict"` なら warning も)・落ちたプレイテスト。polish の指摘と読みにくさは「壊れてはいない」ので blocking にせず、`suggestions` に次の一手として出す(気づけない対象はここで名指しされる)。
- **検査**: `scene`(重い textures / models は既定で外す。`heavy:true` で入れる)/ `layout`(Playing 中は飛ばす。ゲートは直さない = `fix:"none"`)/ `polish` / `ui`(UI が無ければ飛ばす)/ `readability`(`readability` に視点と対象を渡したときだけ。視点ごとに `dx12_perceive` 1 回)/ `playtests`(指定したときだけ。シーンを開き直して再生する)。`checks` で絞れる。検査を足す口は `GATE_CHECKS`(`{id, title, enabled, run}` を 1 つ足せば束ね・合否・keep は共通の仕組みに乗る)。
- **Jev の束ね方**: 既定 `bundle:"perDomain"` は検査ごとの state で 1 リクエストずつ並列に撃つ(待ちは 1 往復ぶん)。`bundle:"one"` は全質問を 1 リクエストにするが、他の検査の事実が混ざって score / choice の判断が落ちる(実測: `ui.brief_fit` の合否 margin 0.78→0.26、`play.cause` 0.98→0.875、`look.next_fix` 0.79→0.74。noul は劣化しない)ので既定にしていない。
- **費用の目安**(本物の Jev、小さなシーン): 既定で 3〜4 リクエスト・6,000〜7,900 トークン・約 $0.0003・Jev の待ち 0.5 秒前後。`bundle:"one"` は 1 リクエスト・約 $0.00025・0.25 秒。
- Brief / 鍵が無いときはルールだけで同じ形を返す(`judge.source:"rules"`、Brief が無ければ `briefMissing:true` と `next` に dx12_brief)。`judge:false` で Jev を使わない。
- **長さ**: blocking / keep / uncertain は「検査:コード」ごとに順繰りに 30 件まで並べる(実機の JUNCTION 丸ごとでは blocking が 307 件 = ほとんど Z_FIGHT で、全部並べると応答が読めなかった)。全件の数は `counts` と `blockingByCode`、省いたら `truncated:true`。個別の一覧は各ツールで見る。
- **実機で確かめたこと**(build/dev の exe をヘッドレス・専用ポートで起動、本物の Jev): JUNCTION のステージ丸ごとでゲート 1.6 秒・Jev 4 リクエスト・約 13,000 トークン・$0.00055、`dx12_perceive` 0.16 秒。

---

### 4-18. 仮想入力モード(AI が人の PC 操作を奪わずエディタ UI を操作・撮影する)

**なぜ**: これまで AI がエディタ UI を触るには実マウス/実キーボードを動かすしかなく、ユーザーがカーソルを奪われて PC を使えなくなった。
仮想入力モードでは、AI の入力を OS を通さずに ImGui へ直接流し込む。

**仕組み**(ON の間):

- **実入力は ImGui に届かない**: `Window::WndProc` が実マウス / 実キーボード / `WM_SETCURSOR` / `WM_INPUT` / フォーカス通知を ImGui にも `InputSystem` にも渡さない
  (Alt+F4 などのウィンドウ管理系は通す)。Win32 バックエンドが NewFrame で積む「実マウス位置 / 修飾キー」も捨てる。
- **AI の入力はキュー → `ImGui::NewFrame` の直前**に、メインスレッドで `io.AddMousePosEvent` / `AddMouseButtonEvent` / `AddMouseWheelEvent` / `AddKeyEvent` / `AddInputCharacter` で流し込む。
  **down と up は必ず別フレーム**(ImGui のクリック判定はフレーム単位)、移動は down の 1 フレーム前。
- **OS のカーソルに触らない**: `SetCursorPos` / `ClipCursor` / `ShowCursor` / `SetCapture` / `SetForegroundWindow` / `SetFocus` / `SetCursor` を全部無効化
  (Play 中のマウスキャプチャ・マテリアル/VFX エディタのオービット・ImGui のカーソル形状変更を含む)。
  ネイティブのファイル選択ダイアログ・`ShellExecute`(エクスプローラー / ブラウザ / VS Code)・別コンソール窓も実行しない(ログに残して失敗扱い)。
- **仮想カーソルを描く**: ImGui の `ForegroundDrawList(メインビューポート)` に矢印 + クリックの波紋(左=青 / 右=橙 / 中=緑)+ 「AI」タグ。スクショに「AI が今どこを触っているか」が映る。
- エディタのフライカメラ(右ボタン + WASD)も仮想ポインタ/仮想キーで動く(前面判定は見ない)。

**起動引数**

| 引数 | 意味 |
|---|---|
| `--virtual-input` | 仮想入力モードで起動(実マウス/キーボードを受けない。窓は普通に出る)。AI 操作専用 |
| `--background[=方式][,tool\|notool]` | **手前に出てこない静かな起動**。仮想入力モードを含意。方式は下表。`--headless` とは別(こちらは窓もレンダリングも普通に動く) |
| `--dpi-scale <0.75〜3.0>` | **表示倍率(DPI)の検証用オーバーライド**(エディタ専用。`1.5` / `150%` / `150` のどれでも可)。OS の表示倍率を無視してその倍率で全体を描く(Windows の設定は一切触らない)。`--background` の窓は論理 1920×1080 × 倍率 の物理サイズになる(例: `--dpi-scale 1.5` → 2880×1620)。指定が無ければ OS の倍率(窓のいるモニターの DPI)に従う |
| `--splash-preview <出力dir> [--dpi-scale N] [--splash-mode startup\|project] [--splash-backdrop mid\|light\|dark]` | **起動画面の見た目の検証**(エディタ専用)。窓を一切表示せず、実窓と同じ描画コードをオフスクリーンで決定論的な時刻/進捗で走らせ、PNG 連番と `frames.csv` を書いて即終了する(音は出さない・D3D12/エンジン本体は初期化しない)。人の画面に起動画面を出さずに確認できる。コンタクトシートは `tools/splash_contact_sheet.py` |
| `--splash-selftest <出力dir>` | 起動画面の実窓コード(専用スレッド・60fps・D2D・UpdateLayeredWindow・Finish の連携)を**窓を表示せずに**通し、フレーム数/描画時間/CPU 使用率を `<dir>/selftest.txt` に書く。終了コード 0 = 合格 |
| `--size <幅>x<高さ>` | **`dx12_screenshot_final` の既定の撮影解像度**(Q2 校正。例 `--size 1920x1080`)。`width`/`height` を省いた撮影が、ビューポート / ウィンドウ / 16:9 レターボックスに依存しないオフスクリーン出力(シーン系 RT をその大きさで作って描き出し、撮影後に元へ戻る)になる。1 辺 16〜8192・総画素 8192x4096 まで(不正な値は無視してログに理由)。詳細は `docs/PARITY_HARNESS.md` §11.6 |
| `--no-splash-sound` | 起動画面の起動音を鳴らさない(環境変数 `UNO_NO_SPLASH_SOUND=1` と、エンジン設定 > 設定 の「起動音」チェック(`%APPDATA%\DX12Engine\editor_state.json` の `startupSound`)でも切れる)。`--background` / `--headless` では元から鳴らさない |

`--background` の共通の挙動: `WS_EX_NOACTIVATE`(クリックされてもアクティブ化しない)/ `SetForegroundWindow` を一切呼ばない / スプラッシュ窓を出さない(起動・プロジェクト読込とも。起動音も鳴らさない)/
自動更新の確認をしない / 最大化・最小化・フルスクリーン・リサイズ・F11 を無効化 / クライアント領域は論理解像度 **1920×1080 × 表示倍率(`--dpi-scale` か OS の倍率。既定 100% なら 1920×1080 の物理 px)** 固定(最小化しても WM_SIZE で縮めない)/
VSync を使わず 60fps 上限(見えていない窓は Present が即返る＝上限が無いと CPU/GPU を回し続けるため。設定は書き換えない)/ OS の省電力(EcoQoS・タイマー粗化)から外れる /
`imgui.ini`(レイアウト)を保存しない(画面外の位置が普段のレイアウトに焼き付かないように)。

| 方式 | 窓の置き場所 | 備考 |
|---|---|---|
| `offscreen`(**既定**) | 全モニタの外(仮想デスクトップの左外)。最小化ではない | クライアント矩形が実サイズのまま保たれ ImGui/スワップチェインが素直に動く。既定で `tool`(タスクバー/Alt+Tab に出ない) |
| `noactivate` | 通常の位置に `SW_SHOWNOACTIVATE` で出し、Z オーダー最背面へ送る | 画面に見える場所に居るが、フォーカスも前面も取らない。他の窓が上を覆う |
| `minimized` | `SW_SHOWMINNOACTIVE`(最小化のまま) | ImGui へは論理解像度を伝える(DisplaySize 0 を防ぐ)。最小化判定は偽装する |
| `hidden` | 窓を一度も表示しない(HWND だけ作る) | `--headless` と同じ「隠し窓」。スワップチェインは可視性を要求しない |

`tool` / `notool` はタスクバー(WS_EX_TOOLWINDOW)の出し分け。例: `--background=noactivate,tool`。

> **★座標は【物理クライアント px】**(`dx12_imgui_pointer` の x/y・`dx12_imgui_find` の rect/center・`dx12_imgui_screenshot` の画像ピクセルはすべて同じ座標系)。
> 表示倍率が 100% 以外のとき、UI の論理サイズ(100% 表示での px)= 物理 px ÷ `dpiScale`。`dx12_ping` / `dx12_imgui_virtual_input` の応答の **`dpiScale`**(1.0 = 100%)で現在の倍率が分かる。
> 倍率が変わる(`--dpi-scale` の違い・OS の設定・別モニターへの移動)と、同じボタンの位置/大きさも物理 px では変わる。**座標を固定値で持ち回らず、毎回 `dx12_imgui_find` で引く**こと。

**ツール**

| ツール | params | 返り値 |
|--------|--------|--------|
| `dx12_imgui_virtual_input` | `{enable?:bool}` | `{enabled, background:{mode,toolWindow}, pointer:{known,x,y,left,right,middle}, queue:{pending,pumped}, client:{width,height}, window:{logicalWidth,logicalHeight,visible,minimized,isForegroundWindow}, osCursor:{x,y}, note}` ※モード切替 + 現在の状態。省略で状態のみ。`osCursor` / `isForegroundWindow` は**読み取り専用**で、「AI の操作中に人のカーソルと前面ウィンドウが動いていないか」を外から確かめる材料 |
| `dx12_imgui_pointer` | `{action:"move"\|"down"\|"up"\|"click"\|"double_click"\|"drag"\|"wheel", x?:f, y?:f, button?:"left"\|"right"\|"middle", toX?:f, toY?:f, steps?:int=12(1..600), dx?:f, dy?:f}` | `{action, at:{x,y}, frames, clamped, pointer:{x,y,left,right,middle}, hover:{window,focusedWindow,wantCaptureMouse,wantCaptureKeyboard,wantTextInput}}` ※**遅延同期**(全部流れて ImGui が反応してから返る)。座標はエディタウィンドウの**クライアント座標(px)**(= `dx12_imgui_screenshot` の画像ピクセル)。`drag` は (x,y)→(toX,toY) を steps フレームで補間。`wheel` は dx/dy をノッチ単位(dy 正 = 上スクロール)、x,y を付けるとその位置へ先に動く。`down`/`up` は x,y 省略で現在位置。領域外は丸める(`clamped:true`。OS ウィンドウを引き出さないため)。`hover.window` で狙った窓に当たったか分かる |
| `dx12_imgui_key` | `{key?:string, text?:string, hold?:int=1(1..600)}` | `{key, vk, hold, text, frames, pointer, hover}` ※**遅延同期**。`key` は `"F2"` / `"Ctrl+S"` / `"Ctrl+Shift+Z"` / `"Enter"` / `"Esc"` / `"Delete"` / `"Up"` / `"PageDown"` / `"A"` 等(修飾は Ctrl/Shift/Alt/Win)。`text` は UTF-8 の文字列をそのまま入力(先にテキスト欄をクリックして `hover.wantTextInput:true` にしておく)。`hold` は key を押し続けるフレーム数(フライカメラの WASD など押している間だけ効く操作用)。ImGui のショートカットと VK ベースの入力(F1 一時停止等)の両方へ届く。Play 中のゲームへは `dx12_key_press` を使う |
| `dx12_imgui_find` | `{label?:string, contains?:bool=true}` | `{client, windows:[{name,title,rect:{x,y,w,h},center,screenRect,visible,focused,hovered,collapsed,docked,popup,dockTabSelected?,tab?:{rect,center}}], items:[{kind,label,window,rect,center,labelRect?}], counts, hover, coordinates}` ※**同期**。ImGui のウィンドウ / ドックのタブ / 名前つき要素を名前で探して矩形を返す(座標を推測しない)。`rect`/`center` はクライアント座標(px)で `dx12_imgui_pointer` にそのまま渡せる。`items` は仮想入力モード ON の間に描かれた名前つき要素(`property`=Inspector 等のプロパティ行[rect は値欄]/ `header`=コンポーネント見出し / `button`=ツールバー / `menu`=メニューバー / `row`=Hierarchy 行)。全ウィジェットは網羅しない — 見つからなければ `windows` の rect を基準にスクショで目視する |
| `dx12_imgui_screenshot` | `{path?:string}` | 画像 + `{path, width, height, source:"backbuffer+imgui", virtualCursor, mode, note}` ※**遅延同期**。**ImGui 込み**(パネル・ギズモ・仮想カーソル)の最終画面を PNG に保存。ImGui を描いた後・Present の前にバックバッファを読み戻す＝**PrintWindow を使わない**ので窓が背面/画面外/最小化でも撮れる。`dx12_ui_screenshot` も仮想入力モード中はこの経路になる。3D の絵だけなら `dx12_screenshot_final` |

**使い方の型**

```
(起動) DX12Engine.exe --background --project C:\path\to\proj --mcp-port 8850
dx12_imgui_find {label:"Inspector"}                  → windows[0].rect / tab を得る
dx12_imgui_pointer {action:"click", x:…, y:…}        → hover.window が "Inspector…" なら当たり
dx12_imgui_find {label:"Position"}                   → items の property 行(rect=値欄)
dx12_imgui_pointer {action:"double_click", x:…, y:…} → 値欄をテキスト編集にする(DragFloat)
dx12_imgui_key {key:"Ctrl+A"} → dx12_imgui_key {text:"1.5"} → dx12_imgui_key {key:"Enter"}
dx12_imgui_screenshot {path:"C:/tmp/shot.png"}      → 仮想カーソル込みで確認
```

**制限・注意**

- **人の脱出口**: 仮想入力モード中は実入力が届かないので、AI が居なくなると人が操作できなくなる。次の 2 つで必ず取り戻せる。
  ① **Ctrl+Alt+Shift+F12**(実キーボード。窓にフォーカスがあるとき)で OFF。② 実行中に MCP が ON にしたモードは、**MCP が 5 秒以上切れたら自動で OFF**。
  起動引数(`--virtual-input` / `--background`)で ON にした物は意図的なので自動では戻らない(`dx12_imgui_virtual_input {enable:false}` で OFF)。
- 仮想入力モード OFF のときは `imgui_pointer` / `imgui_key` を受け付けない(人の実入力と混ざるため)。`imgui_find` / `imgui_screenshot` は OFF でも使える(`items` は ON の間だけ集める)。
- ImGui のダブルクリックは 0.30 秒以内の 2 回押下。フレームが 10fps を切る重い状態では `double_click` が成立しないことがある。
- 仮想ポインタはクライアント領域内に丸める。エディタから引き出した OS ウィンドウ(マルチビューポート)は AI からは触れない(実マウスはそこへは届く)。
- `dx12_imgui_find` の `items` は主要パネルだけ(プロパティ行 / コンポーネント見出し / ツールバー / メニュー / Hierarchy 行)。
- 起動時のプロジェクトランチャー等、ImGui 上のダイアログは普通に操作できる。ネイティブのファイル選択ダイアログは開かない(ブロックしてログに残す)。
- 別プロセスの子エディタ(テストクライアント起動)は同じ `--background` / `--virtual-input` を引き継いで起動する。

### 4-5. 精密ピック / 地形 / スカルプトの約束事

**2 種類のレイキャストを取り違えないこと。**

| | `dx12_raycast` | `dx12_raycast_precise` / `dx12_pick` |
|---|---|---|
| 何に当たるか | Jolt の**物理コライダー** | **描画メッシュの三角形** |
| いつ使えるか | **Playing 中のみ**(body は Play 開始時に登録される) | Editor / Playing どちらでも |
| 精度 | コライダー形状(箱/カプセル/凸包の近似) | 実際の三角形。法線も面の法線 |
| 典型用途 | ゲームロジックの当たり確認・接地判定の再現 | 「スクショのここに何がある？」「地面の実際の高さは？」配置の自動化 |
| 制限 | コライダー形状ぶんの丸め（法線はコライダー面の真の法線） | スキンドメッシュはバインドポーズの AABB 止まり |

`dx12_pick` はエディタの左クリック選択と**同じ `RaycastScene` 実装**を通る（`src/editor/ScenePick.h`）。
MCP で見えるものとエディタで選ばれるものが食い違わないのが、この 2 つを共有している理由。

> ブロードフェーズは**直近に描かれたフレームの描画リスト**を借りる（10 万体でも速いのはこのため）。
> `dx12_set_transform` で動かした直後に撃つと 1 フレームぶん古い位置で判定されることがある。
> 移動 → ピックを続けてやるときは間に `dx12_step_frames(frames:1)` を挟むこと。

**ナビメッシュ（追いかける AI の経路探索）**

- 生成は「ラスタライズ → フィルタ（またぎ/崖/頭上）→ コンパクト化 → エージェント半径ぶん侵食 →
  領域分割（分水嶺 or monotone）→ 輪郭抽出と単純化 → 凸ポリゴン化 → 高さサンプル格子」の自作パイプライン。
  入力は **AABB ではなくメッシュの実三角形**なので、坂道・階段・斜めの壁がそのままの形で反映される。
- **設定はシーン JSON の `navmesh`、焼いた実体はシーンの隣の `<シーン>.nav`**（バイナリ）。
  `dx12_navmesh_build` はメモリ上に焼くだけなので、**残すには `dx12_save_scene` が要る**。
  シーンを開くと `.nav` があれば自動で読む。Play/Stop をまたいでも消えない。
- 除外したいメッシュには `navMeshIgnore` タグを付ける。スキンメッシュ（動くキャラ）は自動で除外。
- **箱や柱のような閉じた立体は、底面と天面の間に頭上クリアランスが空くと内部の床も歩行面として残る**
  （どこからも行けない孤立島になる）。`minRegionArea` を 8〜20 m² に上げると消える。
- Lua からは `nav:ready()` / `nav:sample(pos)` / `nav:findPath(from,to)` / `nav:raycast(from,to)` /
  `nav:moveAlong(from,to)`。`findPath` を毎フレーム全員ぶん呼ばないこと。

**地形（ハイトフィールド）**

- 座標は常に**ワールド XZ**（`point:[x,z]`）。`dx12_pick` の `worldPos:[x,y,z]` をそのまま渡してもよい（y は無視）。
- 高さ配列はシーン JSON に入らない。`assets/terrain/<name>.hf` へ**自動保存**される（彫った次のフレームで書き出す）。
- コリジョンは Jolt の `HeightFieldShape` が**同じ高さ配列を読む**＝彫れば当たり判定も一緒に動く。
- 回転・スケールは効かない（XZ グリッドの前提）。位置だけが意味を持つ。
- 手順は「① `dx12_terrain_create` → ② `dx12_terrain_generate`（土台）→ ③ `dx12_terrain_sculpt`/`dx12_terrain_erode`（詰め）」。
  ②は高さを丸ごと作り直すので、**必ず③より先**にやること。
- **テクスチャレイヤー**（4 層スプラット）は `terrain.layerSetPath` に `.terrainlayers` を割り当てた地形だけ。
  割り当ては **`dx12_terrain_set_layers`**（MCP）/ シーン JSON / 地形ツール窓のどれでもよい。
  割当後は `dx12_terrain_autopaint`（傾斜と標高から焼き直す・冪等）と
  `dx12_terrain_paint`（円ブラシで 1 層を塗る・相対）が使え、結果は
  `dx12_terrain_splat_info` で数値検証できる。詳細は
  [`AUTHORING.md` §10.5.1](AUTHORING.md)。**高さを彫り直したら autopaint をやり直すこと**
  （重みは高さに自動追従しない）。
  手順の例:
  ```
  dx12_terrain_create      {name:"Terrain", resolution:256, worldSize:400}
  dx12_terrain_generate    {name:"Terrain", preset:"mountains", seed:1}
  dx12_terrain_set_layers  {name:"Terrain", layerSetPath:"terrain/alpine.terrainlayers"}   ← ここが無くて詰んでいた
  dx12_terrain_paint       {name:"Terrain", point:[0,0], layer:2, radius:60, strength:1}
  dx12_terrain_splat_info  {name:"Terrain", point:[0,0]}   → weights:[0,0,1,0] で確認
  ```

**スカルプト（異形メッシュ）**

- ハイトフィールドで作れないもの（洞窟・アーチ・せり出した岩）担当。トポロジは変えず頂点だけ動かす。
- `position` は**ワールド座標**で渡すが、`radius` / `strength` は**メッシュのローカル単位**（Transform の scale が掛かる前）。
- 頂点配列は `assets/sculpt/<name>.smsh` へ自動保存。コライダー（`MeshShape`）も彫った形に追従する。

**共通**

- 地形・スカルプトの生成 / 編集系は**すべて Editor 限定**（Playing 中は `MODE_CONFLICT(3)`）。
  Play→Stop はシーンを作り直すので、Playing 中に彫っても巻き戻る。
- メッシュ・コリジョン・`.hf`/`.smsh` の保存が反映されるのは**次のフレーム**（エディタのブラシと同じ経路）。
  彫った直後に見た目を確認するなら `dx12_step_frames(frames:2)` を挟んでから撮ること
  （`dx12_screenshot` は直近に描かれたフレームを返すため）。`dx12_step_frames` は Editor でも使える。

---

## 5. describe_components → set_component の流れ

`set_component` を使う前に `dx12_describe_components` でフィールド定義を確認する。

```
# 1. 使えるコンポーネント一覧を取得
dx12_describe_components({})

# 2. 特定コンポーネントのフィールドを確認
dx12_describe_components({component: "pointLight"})
# → {fields: [{name:"color", type:"vec3", default:[1,1,1]}, {name:"intensity", type:"float", default:1.0}, ...]}

# 3. フィールドに合わせて set_component を実行
dx12_set_component({entity: 42, component: "pointLight", data: {color:[1,0.8,0.6], intensity:3.0, range:10.0}})
```

**tags コンポーネントの例外**: `data` は文字列配列(`["enemy","dynamic"]`)。
**dataComponent**: `data` は `{key: {t:"string", v:"値"}}` 形式のオブジェクト。

---

## 6. idempotency_key

**M5 で write 系の全 method に一般化**(エンジンの冪等ストア = 容量 256・TTL 600 秒。§13-3。別名 `idempotencyKey`)。TS の `dx12_call` の使い方は §0-3。以下は従来からある `create_entity` / `spawn_model` / `spawn_prefab` の固有の挙動(記録した entity が有効ならそれを返す)。

`create_entity` / `spawn_model` / `spawn_prefab` は `idempotency_key` を受け付ける。
同じキーで2回送った場合、2回目は処理をスキップして1回目の `entityId` を
`{"idempotentReplay": true}` 付きで返す。AI がリトライしたときに重複エンティティを防ぐ用途。

```json
{"method":"create_entity", "params":{"type":"box","idempotency_key":"floor-001"}}
```

- `spawn_prefab` は**リプレイ時も `rootEntityId` / `entityIds`（サブツリー全部）を返す**。
- キーはシーンをまたがない（`open_scene` / `new_scene` で表ごと捨てる）。
- 記録された entity が削除済みなら「無かったこと」にして普通に生成する。
- ⚠️ **`spawn_prefab` は 2026-07-26 まで記録だけしてリプレイ判定を持っておらず、
  再送で毎回サブツリーが増えていた**（#20-4 の実バグ。修正済み）。

---

## 7. sceneGeneration（古い entityId の検出）

`sceneGeneration` は `open_scene` / `new_scene` のたびに +1 される整数で、ほぼ全レスポンスに含まれる。
シーンを開き直すと entt レジストリが作り直され、以前の `entityId` は無効になる。

- 無効な `entityId` を使うと `error_code=1(NOT_FOUND)` が返る。
- 解決策: レスポンスの `sceneGeneration` が変わったら、`dx12_ping` で現世代を確認し
  `dx12_list_entities` でエンティティを引き直す。
- `error_code=4(STALE_SCENE)` は将来用に予約済みだが**現状は未送出**（今は `NOT_FOUND` + `sceneGeneration` の変化で判断）。

---

## 8. error_code 一覧

| コード | 定数 | 意味と対処 |
|--------|------|-----------|
| 1 | `NOT_FOUND` | 指定エンティティ/アセット/コンポーネントが存在しない。ID や path を確認。 |
| 2 | `INVALID_PARAM` | パラメータ型・値が不正。`describe_components` でフィールド型を確認。 |
| 3 | `MODE_CONFLICT` | Playing 中に生成系を呼んだ、またはカメラ無しで Play しようとした。先に `dx12_stop` → 再試行。 |
| 4 | `STALE_SCENE` | （予約・現状未送出）シーン再読込での entityId 失効。実際は `NOT_FOUND(1)` が返るので `sceneGeneration` 変化で判断。 |
| 6 | `UNKNOWN_COMPONENT` | `component` の jsonKey が不明。`dx12_describe_components` で有効な jsonKey を確認。 |
| 7 | `INTERNAL` | エンジン内部エラー。`dx12_get_log` でエンジンログを確認。 |

### error_hint / error_values（「次の一手」と有効値）

エラー応答には **任意で** 次の 2 フィールドが乗る（無いこともある。旧来の形は変えていない）。

| フィールド | 内容 |
|---|---|
| `error_hint` | 次に何をすればいいかを 1 文で（例: 「先に `dx12_stop` で Editor へ戻してくれ」）|
| `error_values` | 列挙型の引数が不正だったときの**有効値の全部**（例: `["raise","lower","smooth","flatten","noise"]`）|

Node 側は `Error.hint` / `Error.valid_values` として受け取り、ツールのエラーメッセージへ

```
エラー(code=2): unknown brush: dig
ヒント: 有効値のどれかを指定してくれ
有効な値: raise, lower, smooth, flatten, noise
```

の形で整形して返す。**エラー本文を読めばリトライの引数が決まる**のが狙いなので、
新しいツールを足すときは範囲外・列挙ミスに必ず hint（と可能なら valid_values）を添えること。

---

## 9. 検証ループ

変更をかけた後は以下のループで目視確認できる:

```
1. dx12_set_transform / dx12_set_component で変更
2. dx12_focus_and_screenshot(entity: <entityId>) → PNG で確認
3. dx12_get_log(lines: 30) → エンジンのエラー/警告を確認
4. 問題があれば dx12_undo で自分の直前の変更を戻す(または同じ set_* を反対の値で呼び直す)
```

★**まとまった編集はトランザクションで囲む**: `dx12_transaction_begin(label)` → 編集 → 確かめて良ければ `dx12_transaction_commit`、だめなら `dx12_transaction_rollback`(begin 前へ丸ごと戻る)。`dx12_batch` は既定(`atomic:true`)でこれを自動でやる。
MCP の編集は 1 呼び出し = 1 エントリ「AI: <method>」として Undo に積まれ(2026-09-25 から。以前はほぼ積まれなかった)、応答の `undoEntry` に名前が出る。

★**戻すときは `onlyAi` の既定(true)を信じる**: `dx12_undo` は一番上が人の編集なら戻さずに MODE_CONFLICT(3)を返す。そのときは人に確かめる。人の編集ごと戻すのは `onlyAi:false` を明示したときだけ。

Play/Stop テスト:
```
dx12_play → (ゲームロジック動作) → dx12_stop
→ dx12_focus_and_screenshot でシーン確認
→ dx12_get_log でランタイムエラー確認
```

### 9-1. どのスクショを使うか（絵を判断するときの必読）

| ツール | 撮る先 | 写るもの | 用途 |
|---|---|---|---|
| `dx12_screenshot` | `m_sceneRT`（**ポスト前**） | シーン本体だけ | 幾何 / ライティングの素の値を見たいとき |
| `dx12_screenshot_final` | **バックバッファ**（ポスト後・ImGui 前） | グレーディング / ブルーム / ゴッドレイ / ビネット / LUT / FXAA / デバンド / **TAA 解決結果** | **見た目の判断は必ずこちら** |
| `dx12_ui_screenshot` | ウィンドウ全体（`PrintWindow`）（仮想入力モード中は dx12_imgui_screenshot と同じバックバッファ読み戻し） | 上に加えて ImGui のパネル / ギズモ | エディタ UI・ゲーム内 UI の確認 |

### 9-2. `deterministic` — ピクセル差分で A/B を取るとき（#31）

**同じ設定で 2 回撮っても絵は一致しない。** 実測した原因は 3 つ:

| 原因 | 効く先 | 実測（1920x1032・同一設定 2 枚） |
|---|---|---|
| ポストの **deband ディザ / フィルムグレイン**（`time` 依存の TPDF ノイズ） | `screenshot_final` のみ | 画面の **66%** のピクセルが ±1〜2 LSB |
| **TAA のジッタ**（毎フレーム位相が回るのでラスタ結果そのものが動く） | 両方 | `screenshot` で **9.4%** / max 140 |
| **SSGI・ボリュメトリックフォグの時間ジッタ + 履歴蓄積** | 両方 | SSGI 1.5% / フォグ 5.9% |

`{"deterministic": true}` を付けると:
1. `time` を固定（deband / grain / wave / glitch / パーティクル / カスタムシェーダの time が全部止まる）
2. TAA・フォグ・SSGI の**時間ジッタ位相を毎フレーム 0 に固定**
3. パーティクルの前進を止める（dt=0）
4. **時間蓄積の履歴を捨ててから** `settleFrames`（既定 8）フレーム回し、そこで撮る

→ 実測: 上の全構成（TAA / SSGI / フォグを個別 ON・全部 ON）で **2 枚が完全一致（diff 0.00%）**。

> ⚠️ 止まるのは**レンダラの時間依存だけ**。Play 中のゲームシミュレーション（移動 / 物理 /
> アニメーション）は止まらないので、厳密に比べたいときは `dx12_stop` してから撮ること。
> `settleFrames` を増やすと TAA / SSGI の収束が進む（決定性は 8 でも得られる）。

---

## 10. セキュリティモデル

ブリッジは **エディタ専用**(ゲーム=封印ランタイムでは起動しない)。

- 受けるのは **`127.0.0.1`** のみ。外部ホストからは到達不可。
- 最初の1行が JSON オブジェクト(`{`)で始まらない接続は即切断
  → ブラウザの HTTP/WebSocket ドライブバイ(localhost CSRF)を遮断。
- パス系ツールは **assets 相対のみ**。絶対パス・`..`・`\`・`:` を拒否。
- `create_lua_component` の Lua は書き込み前に構文チェック(コンパイルのみ・非実行)。
- `create_shader` は書き込み前の静的検証ができない(DXC はファイルからしかコンパイルできない)ため、
  書いた後にコンパイルを試し成否を返す方式。失敗してもファイルは書き込まれたまま残る
  (無効なカスタムシェーダーは既定 Forward へ安全にフォールバックするだけで実害は無い)。
- **認証なし(localhost 開発機前提)**。同一マシンの別ユーザプロセスは接続可能なため、
  共有開発機では注意。アップグレード経路: ポートのトークン認証。
- **`dx12_eval_lua` は任意 Lua コードをその場実行する**(意図的な設計。デバッグ効率を優先)。
  上記の認証なしモデルと同水準のリスク(localhost の他プロセスから叩かれれば任意 Lua 実行が可能)。
  ファイルシステムへの直接アクセスは Lua 標準の `io`/`os` ライブラリを sol2 側で公開していない限り
  できないが、エンジンが公開する全バインディング(scene/physics/audio 等)は呼べる。
- `dx12_validate_scene` はエンジン自身を `--validate` 付きで子プロセス起動する。この経路は
  main.cpp で GPU/ウィンドウ/MCP ブリッジの初期化より前に return するため、実行中のエディタと
  ポート等が衝突することはない。

---

## 11. トラブルシュート

| 症状 | 対処 |
|------|------|
| `エディタに繋がらない` | まず `dx12_doctor`(ポート・プロセス・ログを診断して原因と起動コマンドを返す)。エディタが起動しているか・`--mcp-port` と `DX12_MCP_PORT` が合っているか確認。ゲームモードではブリッジ起動しない。 |
| `engine timeout`(`E_ENGINE_TIMEOUT`) | エンジンが処理中か、フレームを回していない(別モーダル等)。エンジンは処理を続けている可能性があるので、撃ち直す前に `dx12_ping` と結果(`dx12_list_entities` 等)を確認する。エディタを前面にする必要は無い(人のカーソルを奪わない `--background` を使う)。 |
| ポート競合 | `%TEMP%\dx12_mcp.port` を読む、または `DX12_MCP_PORT` 環境変数を合わせる。 |
| `node が見つからない` / `.ts` 実行不可 | Node **v24+** を入れる(`node --version` で確認)。 |
| ツールが AI 側に出ない | `claude mcp add` 済みか、`.mcp.json` の `args` パスが正しいか確認。登録後はクライアント再起動。**エンジンに足した method** は再起動不要(§0-1)。 |
| 古い entityId で `NOT_FOUND(1)` | シーンを開き直した。`dx12_ping` で `sceneGeneration` 確認 → `dx12_list_entities` で引き直す。 |
| `MODE_CONFLICT(3)` | Playing 中に生成系ツールを呼んだ。`dx12_stop` してから再試行。 |
| 生成したのに entityId が見つからない | `entityId` をそのまま使う。`name` で検索し直す必要はない(遅延同期で本物の id が返る)。 |
| 別マシンから繋ぎたい | SSH ポートフォワードで localhost に橋渡しし、`DX12_MCP_HOST` + `DX12_MCP_PORT` を合わせる。 |

---

## 12. エンジン内部プロトコル(Node↔エンジン間。参考)

改行区切り JSON、単一 TCP クライアント。

- **リクエスト**: `{"id":<正整数>, "method":<string>, "params":<object>}\n`
- **レスポンス(成功)**: `{"id":<同id>, "ok":true, "result":<any>}\n`
- **レスポンス(失敗)**: `{"id":<同id>, "ok":false, "error":<string>, "error_code":<int>,
  "error_hint":<string 任意>, "error_values":<string[] 任意>}\n`

遅延同期 method は受信時は何も返さず、フレーム境界で実処理後に同じ id でレスポンスを送る。

### 12-1. エンジン側に method を足す（実装者向け）

`Application::HandleMcpCommand` は **`std::unordered_map<std::string, McpMethodEntry>` の表引き**。
以前は `else if (method == "...")` の 118 本連鎖で、MSVC の
**「ブロックの入れ子のレベルが深すぎます (C1061)」上限に張り付いていた**（1 本足すとコンパイルが落ちた）。
表引きなので **method を何本足しても入れ子は 1 段も深くならない**。

足し方は 3 つだけ:

1. `src/core/mcp/ApplicationMcp*.cpp` の `Register***McpMethods()` のどれか（テーマで選ぶ。
   順序に意味は無い）へ。ファイルはテーマ 1 対 1 で分かれている
   （`Entity` / `Editor` / `Render` / `Tooling` / `Asset` / `Terrain` / `Lighting`。
   表の土台と共有ヘルパは `mcp/ApplicationMcp.cpp` と `core/ApplicationInternal.h`）
   ```cpp
   McpDefine("名前", "キー:型,キー:型", DX12E_MCP_HANDLER
       {
           // params / resp / method / deferred / isDeferred / busyPlaying がそのまま使える
           resp["ok"] = true;
           resp["result"] = { ... };
       });
   ```
   `"a|b"` と書くと 1 本のハンドラで 2 つの method を受ける（本文で `method ==` を見て分ける）。
2. 第 2 引数のキー表は **`dx12_describe_mcp_params` がそのまま返す**。型は
   `bool` / `int` / `number` / `string` / `vec3` / `object` / `any`、入れ子は `"親.子"`。
   **本文で読むキーと必ず一致させること。**
   ポスト / SSAO だけは `DX12E_POST_FIELDS` / `DX12E_SSAO_FIELDS`（X マクロ）から自動生成している。
3. TS ラッパ(`tools/mcp-server/toolset/*.ts` の zod スキーマ)を持つ method は、そのスキーマとこのドキュメントにも同じキーを足す
   （**足し忘れると zod が黙って引数を捨てる**。`schemaDrift.test.ts` が見張っている）。ラッパの無い新 method は足さなくても、
   `dx12_tool_describe` → `dx12_call` で MCP サーバの再起動なしに使える(§0-1)。

テーマ別ファイルを新設したときだけ、`src/core/CMakeLists.txt` のソース一覧と
`tests/CMakeLists.txt` の `DX12E_MCP_SOURCES`（`McpParamSpecTests` が走査する対象）にも足すこと。

★`McpDefine(names, "key:type,...", fn)` の形で足したら、`src/core/mcp/ApplicationMcpManifestData.inc` にも同じ名前の行を足すこと
（足し忘れると ctest `McpManifestTests` が落ちる。詳細は 12-2）。新規は `McpDefine(names, McpMeta{...}, fn)` の多重定義でもよい。

- 例外は `throw McpError(McpErr::…, msg, hint, validValues)`。呼び出し側の try/catch が拾う。
- 遅延応答は `deferred` を保存して `isDeferred = true`。
- ハンドラの中で `return;` してよい（旧 else-if 連鎖では `HandleMcpCommand` ごと抜けてしまい
  `RecordCommand` を飛ばしていたので使えなかった）。

### 12-2. マニフェスト（method ごとのメタ情報）

`describe_mcp_params`（引数名と型だけ）とは別に、**カテゴリ・副作用・タイムアウト・引数の詳細**を返す
`describe_mcp_manifest` がある（設計は `docs/MCP_ENHANCEMENT_DESIGN.md` §4.2.2）。
TS サーバはこれを引いて、エンジンを再ビルドしても**再起動せずに**新しい method を呼ぶ。

`describe_mcp_manifest {method?:string, category?:string, brief?:bool}` →

```jsonc
{"ok":true,"result":{
  "protocol":1,
  "manifestHash":"<16 桁 hex>",          // 全 method（名前順）の meta をキー順固定の JSON にして FNV-1a 64
  "engineVersion":"1.x.y",
  "count":175, "total":175,               // count = 返した件数 / total = 全 method 数（絞り込み時に違う）
  "categories":[{"id":"entity","count":21}, ...],   // 常に全 method の集計
  "methods":[{
    "name":"set_ssao", "category":"render", "summary":"…", "keywords":"…",
    "effect":"write_setting",             // read | write_scene | write_setting | write_file | runtime | guarded
    "mode":"any",                         // any | editor | playing
    "timeoutMs":8000, "idempotent":true, "deferred":false,
    "dryRun":"none",                      // none | native
    "group":"render_setting", "target":"ssao",   // 付くものだけ
    "aliases":["dx12_set_ssao"],          // 旧 TS ツール名
    "expose":"core",                      // 付けたときだけ出る（末尾のフィールド）。MCP サーバが tools/list へ動的に昇格させる（M3。guarded は昇格しない）
    "params":[{"name":"radius","type":"number","required":false,"min":0,"max":10,"default":0.5,"desc":"…","enforce":true}],
    "next":[{"tool":"…","when":"…"}], "examples":[{"args":{…},"note":"…"}],
    "source":"meta"                       // meta | derived（paramSpec から自動導出。ctest は 0 件を要求する）
  }]}}
```

- `expose:"core"`（`McpMeta.expose`。既定は空）: TS 側がこの method を Core として `tools/list` に足し `notifications/tools/list_changed` を送る。空のときは canonical JSON にも出ない（既存 method のハッシュを変えない）。
  実機確認用に、環境変数 `DX12_MCP_DEV_PROBE=1` で起動したエンジンだけがダミー method `dev_probe`（`expose:"core"`）を登録する。
- `brief:true` は `params` / `examples` / `next` を省く。`method` 指定は 1 件だけ返し、未知の名前は下記の `E_UNKNOWN_TOOL`。
- `params[].type` は `bool int number string vec2 vec3 vec4 entityRef assetPath enum object array any`。
  `enum`（有効値 `enum`）/ `min` / `max` / `default` / `desc` は付くものだけ。`enforce:true` の引数はディスパッチャが中央検査する（下記）。
- **`manifestHash` は `ping` にも載る**（`manifestHash` / `manifestProtocol` / `engineVersion` / `engineStartedAtMs` / `methodCount`）。
  表を組んだ時に 1 度だけ計算した値で、method や meta が変わると必ず変わり、同じビルドでは毎回同じ。
  TS 側は `ping` の値が前回と違うときだけ `describe_mcp_manifest` を取り直す。`engineStartedAtMs` の変化はエンジンの再起動を示す。

**meta の入れ方（2 通り）**

- 既存 method（`McpDefine(names, "key:type,...", fn)`）は 1 行も書き換えない。meta は
  `src/core/mcp/ApplicationMcpManifestData.inc`（データ表。機械生成した一度きりのブートストラップで、**以後は直接編集してよい**）から
  `ApplyMcpManifest()` が起動後の最初の MCP コマンドで流し込む。表の引数は `enforce:false`（中央検査しない＝既存クライアントを壊さない）。
- 新しい method は `McpDefine(names, McpMeta{...}, DX12E_MCP_HANDLER {...})` の多重定義で書く（`src/core/mcp/McpMeta.h`）。
  `source:"meta"` で、`params` の必須 / 型 / 列挙 / 範囲がハンドラの前に中央検査される。
  `describe_mcp_manifest` 自身がその最初の例（`brief` に文字列を渡すと `E_BAD_TYPE`）。
- ctest `McpManifestTests` が「McpDefine の全 method = データ表の全名」「全 meta が有効」「申告表と表の引数名が一致」を見張る。
  表に行を足し忘れると落ちる（`describe_mcp_manifest` が `source:"derived"` を返す状態を作らない）。

### 12-3. 構造化エラー（加算フィールド）

既存の `error` / `error_code` / `error_hint` / `error_values` は変えない。次のフィールドが**付くものだけ**加算で乗る（旧クライアントは無視できる）。

| フィールド | 内容 |
|---|---|
| `error_name` | 文字列コード（下表） |
| `error_cause` | 原因の 1 文 |
| `error_fix` | `[{tool, args, why}]`。そのまま撃ち直せる次の一手（`tool` は `dx12_` 接頭辞なしの method 名） |
| `error_did_you_mean` | 打ち間違いの近い候補（近い順・最大 5 件。編集距離 + 大文字小文字 + 前方/部分一致） |
| `error_details` | 追加情報（オブジェクト） |

`McpErr` の追加コード（既存の 1〜7 は不変）: `UnknownMethod=8` `Busy=9` `Unsupported=10` `Guarded=11` `Cancelled=12` `ModalOpen=13` `FileIo=14`。
現時点でエンジンが実際に送るのは `8` だけ（残りは TS 側 / 今後の段階用に予約）。

| `error_name` | `error_code` | 出る場所 |
|---|---:|---|
| `E_UNKNOWN_TOOL` | 8 | 未知の method。**`error` は従来どおり `unknown method: X` のまま**、`error_code` だけ 2 → 8 に変えた。`error_did_you_mean` は method 名と別名（`dx12_` を外した名前）から。`error_fix` は `describe_mcp_manifest {brief:true}` |
| `E_NOT_FOUND_ENTITY` | 1 | `name` 指定のエンティティが無い（`error_did_you_mean` はシーン内の名前、大文字小文字違いが先頭）。無効な数値 id も同じ名前。`error_fix` は `list_entities {name_prefix}` |
| `E_NOT_FOUND_SCENE` / `E_NOT_FOUND_ASSET` | 1 | `open_scene` / `spawn_model` / `asset_info` のパス違い（`error_did_you_mean` は実在するパスの近いもの）。`error_fix` は `list_scenes` / `list_assets` |
| `E_STALE_SCENE` | 4 | 全 method 共通の任意キー **`expectGeneration`**（int）が現在の `sceneGeneration` と違う。ハンドラの前に断る（読み取り専用でも）。`error_fix` は `list_entities`。これで初めて `4` が送出される |
| `E_MISSING_PARAM` / `E_BAD_TYPE` / `E_BAD_ENUM` / `E_OUT_OF_RANGE` | 2 | **meta を直接渡した method だけ**の中央検査。`E_BAD_ENUM` は `error_values`（有効値）と `error_did_you_mean`（最も近い値）付き。未知キーは検出しない（互換のため） |

`describe_mcp_params` の `globalKeys` は `["idempotency_key","idempotencyKey","expectGeneration","dryRun","confirm_token"]`（全 method 共通で渡せるキー。M5 で後ろ 3 つを追加）。

---

## 13. 副作用の安全性(M5): guarded ゲート・冪等キー・dryRun プレビュー・ファイルジャーナル

設計は `docs/MCP_ENHANCEMENT_DESIGN.md` §4.3.4。実装は `src/core/mcp/McpSafety.h`(冪等ストア・確認トークン・プレビュー表。ヘッダオンリー)/ `McpJournal.h`・`.cpp`(ファイルジャーナル)/ `ApplicationMcp.cpp`(ディスパッチャ)/ `ApplicationMcpSafety.inc`(guard_token・journal_*・cancel とプレビュー表の本体)。
単体テストは ctest `McpSafetyTests`(エンジン非リンク。時計を注入)。TS 側(`dx12_call` / `dx12_batch`)の使い方は `tools/mcp-server/README.md`。

**ディスパッチャの順序**(`HandleMcpCommand`。meta の `effect` が判定の元):
1. 未知 method / `expectGeneration` / 引数の中央検査(従来どおり)
2. **dryRun**(§13-4): `dryRun:true` で effect が read 以外 → プレビューを返して終わり(ゲート・冪等・実行・未保存フラグ・自動保存カウントダウンのどれも触らない)
3. **冪等キー**(§13-3): キー付き・read 以外 → 前回の結果(Replay)/ 衝突 / 処理中
4. **guarded ゲート**(§13-2): effect が guarded → 有効な `confirm_token` が無ければ拒否
5. ジャーナルのスコープを開く(`meta.journal` が true の method)→ ハンドラ → 閉じる

### 13-1. 共通キー
全 method が次の 3 つを受け付ける(`describe_mcp_params.globalKeys` にも載る)。ハンドラには渡らない(`idempotency_key` は create_entity / spawn_model / spawn_prefab の既存実装のため残る)。

| キー | 型 | 内容 |
|---|---|---|
| `dryRun` | bool | true で実行せず「何が起こるか」を返す(§13-4)。read は無視して通常実行 |
| `confirm_token` | string | guarded な method の確認トークン(§13-2) |
| `idempotency_key` | string | 冪等キー(§13-3)。別名 `idempotencyKey` も同義(両方あれば `idempotency_key` が優先) |

### 13-2. guarded ゲート(エンジン側の最終関門)
- 対象 = `meta.effect == guarded` の 11 件: `git_checkout` `git_commit` `git_fetch` `git_merge` `git_merge_abort` `git_pull` `git_push` `eval_lua` `delete_asset` `build_game` `net_launch_test_client`(`git_status` / `git_branches` は read)。
- **`guard_token {method}`**(effect=runtime)→ `{token, method, ttlSec}`。トークンは **1 回限り・method 束縛・TTL 60 秒(環境変数 `DX12_MCP_GUARD_TTL_SEC` で縮められる。テスト用)・保持は最大 32 個**(古い順に捨てる)。128bit の乱数(hex 32 文字)。guarded でない method を渡すと `E_INVALID_PARAM`(2)+`error_values`(guarded の一覧)+`error_did_you_mean`。
- トークン無し / 未知 / 使用済み / 期限切れ / 別 method 用 → `error_code:11`・`error_name:"E_GUARDED"`・`error_cause`(理由)・`error_fix:[{tool:"guard_token"}]`・`error_details:{method, gate:"engine"}`。ハンドラは呼ばれない。検査に通った(または見つかった)トークンはその場で消費される。
- **dryRun:true はゲートより先**に処理する(トークン不要・実行しない)。guarded でプレビューを持つのは `delete_asset` だけ。他の guarded は `E_UNSUPPORTED`。
- **冪等の Replay はゲートより先**(前回の結果を返すだけなのでトークン不要。再実行しない)。
- TS の `dx12_call_guarded` / `dx12_call {confirm:true}`(full / shell 面)は、ユーザーの承認を通った後に `guard_token` → `confirm_token` 付きの呼び出しを自動で行う。`dx12_batch` の op はトークンを持たないので guarded を実行できない(エンジンが拒否する)。
- ⚠ 認証ではない(127.0.0.1 限定・認証なしのまま。`§10`)。「確認を通っていない呼び出しを機械的に止める」ためのゲートで、悪意のあるローカルプロセスを止めるものではない。

### 13-3. 冪等キー(write 系全般・有限サイズ・TTL)
- 対象 = effect が read 以外の全 method(`create_entity` / `spawn_model` / `spawn_prefab` は既存の `m_mcpIdempotency` 実装のまま。`guard_token` と `cancel` は対象外)。
- ストア = **容量 256(LRU)・TTL 600 秒**。エントリ = `{key, method, paramsHash, state:InFlight|Done, 応答}`。`paramsHash` は `dryRun` / `confirm_token` / `idempotency_key` / `idempotencyKey` を除いた params の canonical dump の FNV-1a64(キーの順序に依らない)。
- 同じキー:
  - Done で method・引数が同じ → **ハンドラを実行せず前回の応答を返す**(`result.idempotentReplay:true`・`result.idempotency:{key, firstAtMs}`。`id` は今回のもの)。Undo エントリも未保存フラグも増えない。
  - method か引数が違う → `error_code:2`・`error_name:"E_IDEMPOTENCY_CONFLICT"`(`error_details.firstMethod`)。
  - 処理中(InFlight。遅延 method の完了前・120 秒で期限切れ)→ `error_code:9`・`error_name:"E_IDEMPOTENCY_IN_FLIGHT"`。少し待って同じ内容で再送すれば Replay になる。
- 実行する場合は先に InFlight を積み、成功(`ok:true`)で Done・失敗なら削除(再送で再実行できる)。**遅延 method**(`delete_entity` / `open_scene` / `play` など、フレーム境界で応答が出るもの)は、`McpBridge::SendToClient` の観測点(`mcpsafety::SendObserver`)で「(client, requestId) の応答が出た」ことを拾って Done / 削除にする(`CompleteMcp` / `FailMcp` は触っていない)。
- `open_scene` / `new_scene` / `open_project` / `play` / `stop` を**実行するとき**はストアを空にする(シーンが入れ替わるため。自分の Done は完了後に積まれる)。Replay になる場合は空にしない。

### 13-4. dryRun(実際に何が起こるかを実行せず返す)
`dryRun:true` + effect が read 以外の method:
- プレビュー表にある method(`meta.dryRun == "preview"`。マニフェストの `dryRun` に出る)→ `{ok:true, result:{dryRun:true, executed:false, method, effect, preview}}`。
  `preview = {summary, targets[], count, destructive, undoable, files[], notes[], willFail?}`。`targets` は `{kind:"entity"|"file"|"scene"|"journal", exists, id?, name?, …}`、`files` は `{path, exists, action:"create|overwrite|delete|move|read", bytes?}`。`willFail:true` は「実行すると失敗する」(対象が無い・構文エラー・上書き不可など。`reason` / `didYouMean` 付き。dryRun 自体は ok:true)。
- 無い method → `error_code:10`・`error_name:"E_UNSUPPORTED"`・`error_values`=対応 method の一覧。
- read は `dryRun` を無視して通常実行する。
- **副作用ゼロ**: シーン・ファイル・Undo・冪等ストア・未保存フラグ・自動保存カウントダウンのどれも変えない(実機で前後のシーン全状態とアセットツリーのハッシュ一致を確認)。

| method | プレビューの内容 |
|---|---|
| `create_entity` | 生成される type・名前(省略時は自動)・位置・親。type が不正なら willFail |
| `spawn_model` / `spawn_prefab` | path の実在・生成名。無ければ willFail(プレハブは階層ぶん増える旨) |
| `delete_entity` | 対象+子孫の件数と名前(先頭 20)。destructive |
| `duplicate_entity` | 対象+子孫の件数(増える数) |
| `set_transform` | 各フィールドの現在値 → 新しい値(`targets[0].changes`) |
| `set_component` | 既存コンポーネントへのマージか新規追加か・上書きされるフィールドの現在値 → 新しい値 |
| `remove_component` | 付いているか・destructive |
| `set_parent` / `rename_entity` | 親の前後 / 名前の前後(連番が付くか・名前参照が追従して書き換わる旨) |
| `save_scene` | path・既存の上書きか新規か・現在のバイト数・エンティティ数・sceneDirty |
| `open_scene` | path の実在・現在のシーンを閉じる・未保存の警告 |
| `create_lua_component` / `create_shader` | 書き込み先・上書きか新規か・(Lua は)構文検査の結果 |
| `move_asset` / `delete_asset` / `import_asset` | 件数・総バイト・移動先/取り込み先が既にあるか・(delete)まだ参照されているか・破壊性 |
| `journal_restore` | 戻る / 消えるファイルの一覧・エントリが不完全か |

### 13-5. ファイル書き込みジャーナル(`<project>/.dx12/journal/`)
- **何のため**: ファイルを書く操作は Undo の対象外で、トランザクションを rollback してもファイルは戻らなかった。書く直前に「上書き・削除・移動される(または新規作成される)ファイル」の元の内容を退避し、rollback / `journal_restore` で書き戻す。
- **対象**(`meta.journal:true`): `save_scene` `create_lua_component` `create_shader` `move_asset` `delete_asset`(フォルダは配下を 1 ファイルずつ) `import_asset` `create_prefab` `journal_restore`。TS の `dx12_scene_write` は Node が書くので TS 側が同じ形式のエントリを書く。
- **形式**: `<root>/<seq 6 桁>-<method>/manifest.json` + `files/<n>.bin`。`manifest.json` = `{version:1, id, method, label, createdAt(ms), state:"open|committed|rolledBack|restored", txLabel|null, complete, note?, files:[{path(project 相対・外は絶対・区切りは '/'), existed, backup("files/0.bin"|null), bytes, skipped(null|"too_large"|…)}]}`。seq は既存フォルダの最大 + 1。
- **適用単位**: トランザクション中は 1 つの tx エントリにまとめる(`transaction_begin` で開く → `transaction_commit` で state:committed のまま保持 / `transaction_rollback` で**シーンの巻き戻しと一緒にファイルも戻して** state:rolledBack。人の Play などによる確定扱いの自動 close は committed)。tx 外は journal 対応 method の 1 呼び出し = 1 エントリ(1 つも退避しなければフォルダを作らない)。commit / rollback の応答に `journal:{id, restored, unchanged, missing, warnings, complete}` が付く。
- **上限**: 1 ファイル 64MB 超は退避しない(`skipped:"too_large"`・`complete:false`)。フォルダの退避は 1 エントリ 2000 ファイル / 256MB まで(超えたら `complete:false`+note)。保持は閉じたエントリ 50 件(古い順に刈る。open は刈らない。別プロセスが残した 24 時間より古い open は刈る)。`move_asset` が参照書き換えで触った他ファイル(他シーン・.prefab 等)は退避しないので `complete:false` とその旨を note に残す。
- **`journal_list {limit?}`**(read)→ `{entries:[{id,method,label,state,createdAt,fileCount,complete,txLabel,note}], dir, total}`(新しい順)。
- **`journal_restore {id}`**(write_file・dryRun=preview・journal)→ `{restored[], unchanged[], missing[], warnings[], complete}`。existed:false のファイルは削除、existed:true は書き戻す。現在の内容が既に同じなら何もしない(unchanged)。**復元自体も 1 エントリとして残る**(復元の復元ができる)。開いているシーンのメモリには反映されない(シーンを戻したなら `open_scene` で開き直す)。`..` を含む相対 path は戻さない(警告)。TS が書いたエントリも同じ形式で復元できる。
- **未保存フラグ**: `guard_token` / `journal_list` / `journal_restore` / `cancel` はシーンのメモリを変えないので sceneDirty を立てない(`IsMcpReadOnlyMethod`)。`journal_restore` を入れないと、戻した現在シーンのファイルを自動保存がメモリ上のシーンで上書きして復元が無かったことになる(実機で検証)。
- **自動保存**: AI のトランザクションが開いている間は本保存(2 秒アイドル)を保留する(rollback で戻す途中の状態をディスクへ書かない)。閉じた後に通常どおり保存される。

### シーンの世代つきバックアップ(`scene_backups`)

保存のたびに直前のシーン一式を `<project>/.dx12/backups/` へ世代として残す。実体は `objects/<内容ハッシュ>-<大きさ>`（同じ内容は 1 回だけ。置き換えで退いた古いファイルは改名で移すのでコピーが要らない）、世代は `<シーン名>_<YYYYmmdd_HHMMSS>.gen`（相対名→ハッシュの目録。最後に原子的に書く＝一覧に出る世代は完全）。現行ファイルを外部がその場で上書きしても世代は変わらない。世代を消すと参照されなくなった実体だけ消える。既定 10 世代・合計 1024MB・60 秒間隔（プロジェクトの `settings.json` の `backup_enabled` / `backup_generations` / `backup_max_mb` / `backup_interval_sec`）。.autosave など「.」で始まるフォルダは対象外。

| method | 引数 | 返り値 |
|---|---|---|
| `scene_backups` | `{op:"list"|"restore"|"snapshot"|"settings", path?, id?, enabled?, generations?, maxMb?, intervalSec?}` | list: `{scene, dir, policy, count, generations:[{id, time, bytes, hasParts, hasInst, hasNav, file}], loadFailed}` / restore: `{restored, scene, note}`（戻すシーンは次のフレームで読み直す。戻す前の版も 1 世代残る）/ snapshot: `{created, id, why}` / settings: `{policy, changed, dir}` |

シーンが壊れて開けないとき（`open_scene` が `scene load failed`）は、空のシーンで本体を上書きしないよう保存・自動保存を止めてある。`scene_backups list` → `restore` で戻す。エディタはファイル メニュー「以前の版に戻す…」。

### 13-6. `cancel`(M6 のジョブ API の口)
`cancel {target?:"benchmark"|"step_frames"|"all"(既定 all)}`(effect=runtime・冪等キー対象外)。実行中の `benchmark`(`m_benchFramesLeft`)/ `step_frames`(`m_mcpStepFramesLeft`)の残りを 1 フレームに切り詰め、**次のフレームで保留中の遅延応答が正常な完了として返る**(benchmark は途中までの統計・step_frames は `simulatedSec` が要求より短い)。応答 `{cancelled:[…], framesLeft:{benchmark?, step_frames?}(切り詰める前の残り), note}`。何も走っていなければ `cancelled:[]`。

### 13-7. `ping.safety`(加算)
`{guardedGate:true, idempotency:{entries, capacity, ttlSec}, journal:{dir, entries}}`。

### 13-8. エラー名(エンジンが `error_name` で送る。TS の `errors.ts` が受ける)
| `error_name` | `error_code` | 意味 |
|---|---:|---|
| `E_GUARDED` | 11 | guarded の method を有効な確認トークン無しで要求した |
| `E_IDEMPOTENCY_CONFLICT` | 2 | 同じ冪等キーが別の method / 別の引数で使われた |
| `E_IDEMPOTENCY_IN_FLIGHT` | 9 | 同じ冪等キーの前回の要求がまだ完了していない(少し待って再送) |
| `E_UNSUPPORTED` | 10 | dryRun のプレビューを持たない method |

---

関連: `tools/mcp-server/AGENTS.md`(AI エージェント運用ガイド)、`tools/mcp-server/README.md`(サーバ構成)、
`docs/AUTHORING.md`、`docs/SCRIPT_COMPONENTS.md`(Lua)。
