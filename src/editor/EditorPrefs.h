#pragma once

// ===== エディタ設定の永続化（ユーザー単位・プロジェクトを跨ぐ）=====
// 保存先: %APPDATA%\DX12Engine\editor_state.json の "prefs" オブジェクト（環境変数 DX12E_DATA_DIR で分離される）。
//   他機能が同じファイルに書くキー（lastOpenedScene など）は消さない。
// 使い方:
//     if (prefs::GetBool("asset.listView", false)) ...
//     prefs::SetInt("asset.iconSize", 96);     // 値が変わった時だけ保存が予約される（1.5 秒のデバウンス）
// ・毎フレーム呼んでよい（ハッシュマップ引き。同じ値の Set は何もしない）。
// ・キー名は "<領域>.<名前>"（例 hier.typeFilter / insp.recentComponents / asset.listView / layout.ratios）。
// ・EditorLayer::Render が毎フレーム Tick() を呼ぶ。終了時は FlushNow()（EditorLayer のデストラクタ）。
// ・純ロジックは editor/EditorPrefsStore.h（tests/editor_prefs_test.cpp）。

#include <string>
#include <vector>

namespace dx12e::prefs
{

bool        GetBool(const char* key, bool def);
int         GetInt(const char* key, int def);
float       GetFloat(const char* key, float def);
std::string GetString(const char* key, const std::string& def = {});
bool        Has(const char* key);
std::vector<std::string> KeysWithPrefix(const std::string& prefix);

void SetBool(const char* key, bool v);
void SetInt(const char* key, int v);
void SetFloat(const char* key, float v);
void SetString(const char* key, const std::string& v);
void Erase(const char* key);

void Tick();       // 毎フレーム 1 回。dirty でデバウンス時間が過ぎていれば書き込む
void FlushNow();   // 即時に書き込む（終了時）

// --background / UI 自動テスト / 決定論撮影では書き込みを止める（メモリ上の値は変わる）。
void SetWriteEnabled(bool on);

// テスト用: 保存先ファイル(editor_state.json の代わり)を差し替える。空で本番の配線へ戻す。
void SetPathForTests(const std::string& utf8Path);

} // namespace dx12e::prefs
