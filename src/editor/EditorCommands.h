#pragma once

// ===== エディタのコマンド実行・ショートカット処理・メニュー描画 =====
// 表（何のコマンドがあるか・どのキーか）は EditorCommandTable.h / ToolWindows.h / EditorCreateTable.h。
// ここはその表を「実際に動かす」側:
//   ・ProcessShortcuts : 毎フレーム 1 回。表のキーが押されたら Execute する（Application に散っていた処理の移設先）。
//   ・Execute          : コマンド id を実行する（メニュー / ショートカット / パレットが全部ここを通る）。
//   ・MenuItem         : 表のラベル・アイコン・キー表記でメニュー項目を描く（表記が実装と食い違わない）。
// 実際のシーン操作は EditorContext の pending* フラグ経由（Application / ToolbarPanel / EditorLayer が消費）。

#include "editor/EditorCommandTable.h"
#include "editor/EditorCreateTable.h"

#include <DirectXMath.h>
#include <string>
#include <string_view>

namespace dx12e
{
class EditorContext;
class Scene;
}

namespace dx12e::cmd
{

struct Env
{
    Scene*      scene = nullptr;   // コピー / 貼り付けのシリアライズに使う
    std::string assetsDir;
};

// id に対応するアイコン（EditorIcons.h の ICON_*）。無ければ ICON_BLANK。
const char* IconFor(std::string_view id);

// メニュー右側に出すキー表記。無ければ nullptr（ImGui::MenuItem の shortcut 引数へそのまま渡せる）。
// 戻り値は静的領域（呼び出し側で解放しない）。
const char* ShortcutText(std::string_view id);

// 今の状態で実行できるか（Play 中は無効 / 選択が無いと無効 など）。
bool IsEnabled(const EditorContext& ctx, std::string_view id);
// トグル系（照らし込み / キーボードフライ / 2D ビュー）の ON 状態。
bool IsChecked(const EditorContext& ctx, std::string_view id);

// 実行。"window.<toolId>" は窓の開閉トグル、"create.<...>" はエンティティ作成、それ以外は表のコマンド。
// 未知の id は何もしない（false を返す）。
bool Execute(EditorContext& ctx, const Env& env, std::string_view id);

// ショートカットの処理。EditorLayer::Render から毎フレーム 1 回。
void ProcessShortcuts(EditorContext& ctx, const Env& env);

// 表の項目をメニューに描く。押されたら Execute して true。toggle 系は自動で ✓ を付ける。
bool MenuItem(EditorContext& ctx, const Env& env, const char* id);

// ---- エンティティ作成 ----
// カメラの前（下を向いていれば地面との交点）の位置。defaultY はそのまま Y に使う。
DirectX::XMFLOAT3 SpawnPosition(const EditorContext& ctx, float defaultY);
// 表の項目を実際に作る（PendingSpawnRequest を積む / 専用窓を開く）。
void SpawnItem(EditorContext& ctx, const CreateItem& item);
// 作成メニューの中身（グループの区切り・Gimmick / UI のサブメニュー込み）。
// BeginPopup の中で呼ぶ。ヒエラルキーの「エンティティ追加」と空白部分の右クリックが共用する。
void DrawCreateMenu(EditorContext& ctx);

} // namespace dx12e::cmd
