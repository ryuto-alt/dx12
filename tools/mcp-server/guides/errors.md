# エラーの読み方と直し方
> `fix[0]` をそのまま `dx12_call` に渡して撃ち直す。同じ呼び出しを繰り返さない。

## 形
```json
{"ok":false, "error":"…", "error_code":"E_BAD_ENUM", "engineCode":2, "cause":"…", "retryable":false,
 "didYouMean":["sculpt"], "validValues":["create","generate","sculpt"],
 "fix":[{"tool":"dx12_terrain_sculpt","args":{"brush":"lower"},"why":"…"}],
 "details":{…}, "docs":"dx12_tool_describe {name:'…'}"}
```
旧ツールを直接呼んだ場合は、従来の日本語本文の後ろに同じ JSON が 1 ブロック付く。

## コード表
| error_code | 意味 | 直し方 |
|---|---|---|
| E_ENGINE_UNREACHABLE | エンジンに繋がらない | `dx12_doctor`。起動は `--background` |
| E_ENGINE_BUSY | 接続は通るが応答が無い(別セッションが保持/処理中) | 他セッションを閉じる。少し待つ |
| E_ENGINE_TIMEOUT | 期限内に応答が来ない。エンジンは処理を続けている可能性 | `dx12_ping` で応答確認 → 結果を確かめてから撃ち直す(生成系は二重生成に注意。`idempotency_key` を使う)。遅れて届いた結果は次の `meta.lateResults` |
| E_ENGINE_TOO_OLD | エンジンが古い | エンジンを更新・再ビルドして再起動(MCP の再起動は不要) |
| E_UNKNOWN_TOOL | ツール/メソッドが無い | `didYouMean` / `dx12_tool_search` |
| E_UNKNOWN_PARAM / E_MISSING_PARAM / E_BAD_TYPE / E_BAD_ENUM / E_OUT_OF_RANGE | 引数不正 | `fix[0].args`(機械的に直した引数)/ `validValues` |
| E_NOT_FOUND_ENTITY / E_NOT_FOUND_ASSET / E_NOT_FOUND_SCENE / E_NOT_FOUND_COMPONENT | 対象が無い | `didYouMean`(近い名前)/ 一覧ツール(dx12_list_entities / dx12_list_assets / dx12_list_scenes / dx12_describe_components) |
| E_STALE_SCENE | `expectGeneration` が古い | `dx12_list_entities` で引き直す(name 指定なら世代に影響されない) |
| E_MODE_CONFLICT | Editor/Playing が合わない・トランザクション中に禁止 | fix の `dx12_stop` などを先に撃って同じ呼び出しを再送 |
| E_VIRTUAL_INPUT_OFF / E_MODAL_OPEN | 仮想入力が OFF / モーダルが開いている | `dx12_imgui_virtual_input {enable:true}` |
| E_UNSUPPORTED | 環境が非対応(DXR など) | 再送しても無駄。代替を使う |
| E_GUARDED | guarded な操作に `confirm` が無い(core 面は `confirm:true` でも不可) | 承認を得て `confirm:true`(core 面は `dx12_call_guarded`) |
| E_FILE_IO | ファイル書込/読込失敗 | `path` を明示する |
| E_VALIDATION_FAILED | 宣言的な入力(シーン JSON など)の検証失敗 | `issues[]` の path と fix で該当箇所を直す |
| E_CANCELLED | 呼び出しが中断された | 同じ呼び出しを撃ち直せる |
| E_SAFETY_VIOLATION | 仮想入力中に OS のカーソル/前面窓が動いた(本来起きない) | 以降の UI 操作を止めて人に報告する |
| E_INTERNAL | エンジン内部エラー | `dx12_get_log` で直前のログ |
| E_NOT_FOUND / E_INVALID_PARAM | 種類を特定できなかった旧経路 | メッセージと hint を読む |

`retryable:true` は「原因を除けば同じ呼び出しが通る」、`false` は「引数か状態を直さないと通らない」。
