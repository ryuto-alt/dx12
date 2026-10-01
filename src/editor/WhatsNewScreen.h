#pragma once

// ===========================================================================
// 「更新内容」画面（更新後の初回起動 / コマンド「更新内容を表示」）。ImGui のモーダル 1 枚。
//   ・見出し「v1.19.0 → v2.0.0 に更新しました」+ 日付・見出し文
//   ・注目カード（2 列）/ 種類別の一覧（新機能・改善・修正。色つきラベルと件数）
//   ・複数の版を飛ばして更新したときは、間の全ての版を新しい順にまとめて（版ごとに折りたたみ）
//   ・フッタ「GitHub で全文を見る」「閉じる」。本文はスクロール領域で、フッタは必ず見える
// データは core/ReleaseNotes.h（唯一の正）。色・寸法はテーマのトークンと ui::Px だけ。
// 呼び出し側（Application::RenderWhatsNewPopup）が OpenPopup / 表示済みの記録 / URL を開く処理を持つ。
// ===========================================================================

#include <string>
#include <vector>

#include "core/ReleaseNotes.h"

namespace dx12e::whatsnew
{

struct State
{
    std::string from, to;                          // 前回表示した版（空 = 初回 / 手動）→ 今の版
    bool manual = false;                           // true なら全履歴（手動表示）
    std::vector<const relnotes::Release*> releases;   // 新しい順
    std::vector<char> expanded;                    // 版ごとの開閉
};

// 表示する版の並びを決める。manual=true は to 以下の全履歴（先頭だけ開く）、false は from→to の間（先頭だけ開く）。
void Setup(State& s, const std::string& from, const std::string& to, bool manual);

enum class Action { None, Close, OpenGithub };

// BeginPopupModal の中で呼ぶ。押されたボタンを返す（Esc は Close）。
Action Draw(State& s);

// 見出しの文言（テスト・撮影用）。例: "v1.19.0 → v2.0.0 に更新しました" / "v2.0.0 の更新内容"。
std::string TitleText(const State& s);

} // namespace dx12e::whatsnew
