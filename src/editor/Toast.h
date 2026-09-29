#pragma once

// ===== トースト通知（右下に積む・自動で消える）=====
// どこからでも呼べる（ImGui にも EditorContext にも依存しない。ヘッダは軽い）:
//     ui::Toast(ui::ToastKind::Success, "保存しました: stage1.json");
//     ui::ToastInfo("コピーしました");  ui::ToastWarn(...);  ui::ToastError(...);
// EditorContext からは ctx.Notify(kind, msg) でも同じ。
//
// 使い分け（既存の「緑文字 1.5 秒」「赤文字」「中央モーダル」の代わり）:
//   ・結果を知らせるだけ（保存した / コピーした / 失敗した）→ トースト。
//   ・ユーザーの判断が要るもの（未保存の確認 / 削除の確認）→ モーダルのまま。
// 表示は EditorLayer が毎フレーム ui::RenderToasts() を呼ぶ（editor/Toast.cpp）。
// スレッド: 別スレッド（git 非同期操作など）から呼んでよい（内部で排他）。
// MCP の書き込み系は呼ばない（AI がモーダルやトーストを待たない既存方針。ただし MCP の notify は別に用意）。

#include "editor/ToastQueue.h"

#include <string>

namespace dx12e::ui
{

// 積む。seconds <= 0 で既定（通常 3 秒 / Error 6 秒）。戻り値は id。
uint32_t Toast(ToastKind kind, std::string message, float seconds = -1.0f);

inline uint32_t ToastInfo(std::string m)    { return Toast(ToastKind::Info,    std::move(m)); }
inline uint32_t ToastSuccess(std::string m) { return Toast(ToastKind::Success, std::move(m)); }
inline uint32_t ToastWarn(std::string m)    { return Toast(ToastKind::Warn,    std::move(m)); }
inline uint32_t ToastError(std::string m)   { return Toast(ToastKind::Error,   std::move(m)); }

// 毎フレーム 1 回。右下（ステータスバーの上）へスタック描画し、クリックで閉じる。
// bottomInset: 画面下端から空ける高さ（ステータスバーぶん）。
void RenderToasts(float dt, float bottomInset);

// 現在の生きている枚数（UI 自動テスト用）。
size_t ToastLiveCount();
// 全部消す（テスト用）。
void ToastClearAll();

// 種別 → 文字列（"info" / "success" / "warn" / "error"）と逆変換（MCP の notify 用）。
const char* ToastKindName(ToastKind k);
bool ParseToastKind(const std::string& s, ToastKind& out);

} // namespace dx12e::ui
