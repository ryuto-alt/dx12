#pragma once

// ===== Nebula Engine Editor ライクなダークテーマのパレット =====
// claude.ai/design の "Nebula Engine Editor" デザインから抽出した配色を
// エディタ全体（ImGui スタイル + 各パネルのアイコン tint / アクセント）で
// 共有するための単一ソース。ヘッダオンリー（リンク依存なし）。

#pragma warning(push)
#pragma warning(disable: 4201)
#include <imgui.h>
#pragma warning(pop)

namespace dx12e::theme
{

// 0xRRGGBB → ImVec4（sRGB のまま。ImGui の頂点カラーに合わせてガンマ変換はしない）
inline ImVec4 Hex(unsigned int rgb, float a = 1.0f)
{
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f,
                  ((rgb >> 8)  & 0xFF) / 255.0f,
                  ( rgb        & 0xFF) / 255.0f,
                  a);
}

// ---- 背景（深 → 浅）----
inline const ImVec4 AppBg       = Hex(0x0e0f12);  // 最深部（ドック空き / ビューポート外のレターボックス）
inline const ImVec4 PanelBg     = Hex(0x16171b);  // パネル本体
inline const ImVec4 Chrome      = Hex(0x1a1b20);  // ツールバー / タブバー等のクロム
inline const ImVec4 GroupBg     = Hex(0x202127);  // セグメント / ボタン群の地
inline const ImVec4 FrameBg     = Hex(0x1d1e23);  // 入力欄・スライダー溝
inline const ImVec4 FrameBgHi   = Hex(0x242530);
inline const ImVec4 FrameBgActive = Hex(0x2b2c38);
inline const ImVec4 Border      = Hex(0x25262c);  // パネル境界（控えめ）
inline const ImVec4 BorderSoft  = Hex(0x2c2d34);  // フレーム枠（少し明るい）

// ---- アクセント（青）----
inline const ImVec4 Accent      = Hex(0x4c8dff);
inline const ImVec4 AccentLight = Hex(0x6ba2ff);
inline const ImVec4 AccentDim   = Hex(0x4c8dff, 0.18f);  // 選択行の薄膜
inline const ImVec4 AccentDim2  = Hex(0x4c8dff, 0.32f);

// ---- テキスト ----
inline const ImVec4 TextHi      = Hex(0xe6e7ea);
inline const ImVec4 TextMid     = Hex(0xc6c8cf);
inline const ImVec4 TextDim     = Hex(0x9a9ca4);
inline const ImVec4 TextFaint   = Hex(0x74767f);

// ---- 種別カラー（Hierarchy / Inspector のアイコン tint）----
inline const ImVec4 TypeMesh    = Hex(0x9aa0ad);
inline const ImVec4 TypeLight   = Hex(0xf5b740);
inline const ImVec4 TypeCamera  = Hex(0x3fb6b0);
inline const ImVec4 TypeAudio   = Hex(0x5fc77e);
inline const ImVec4 TypeScript  = Hex(0xa98cff);
inline const ImVec4 TypePhysics = Hex(0xe08a5b);
inline const ImVec4 TypeUi      = Hex(0xec4899);
inline const ImVec4 TypePrefab  = Hex(0x6ba2ff);
inline const ImVec4 TypeEmpty   = Hex(0x8c8f98);

// ---- ステータス ----
inline const ImVec4 Good        = Hex(0x6fcf45);
inline const ImVec4 Warn        = Hex(0xf5b740);
inline const ImVec4 Bad         = Hex(0xe5564e);

} // namespace dx12e::theme
