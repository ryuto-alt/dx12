#pragma once

// ===========================================================================
// --background: 「手前に出てこない静かな起動」の方式（純粋ロジック）
// ---------------------------------------------------------------------------
// AI（MCP）がエディタを起動して操作するとき、窓が手前に描画されて人の作業を邪魔しない。
// 起動引数:
//   --background                 既定方式（offscreen）
//   --background=offscreen       画面外（全モニタの外側）に置く。最小化ではないのでクライアント矩形が
//                                実サイズのまま保たれ、ImGui / スワップチェインが素直に動く【既定】
//   --background=minimized       SW_SHOWMINNOACTIVE（最小化のまま起動）。論理解像度で動かす
//   --background=noactivate      普通の位置に SW_SHOWNOACTIVATE で出し、Z オーダーの最背面へ送る
//   --background=hidden          窓を一切表示しない（HWND は作るが Show しない。--headless と同じ発想）
//   カンマ区切りで修飾: `tool`（WS_EX_TOOLWINDOW=タスクバー/Alt+Tab に出ない）/ `notool`
//   例: --background=offscreen,tool
//   ★どの方式でも WS_EX_NOACTIVATE を付ける（クリックされてもアクティブ化しない）。
//   ★--background は仮想入力モード（--virtual-input）を含意する（実入力を奪わないため）。
//
// GPU / Win32 に依存しない（tests/virtual_input_test.cpp から使う）。
// ===========================================================================

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace dx12e
{

enum class BackgroundMode { None, Offscreen, Minimized, NoActivate, Hidden };

struct BackgroundOptions
{
    BackgroundMode mode = BackgroundMode::None;
    bool           toolWindow = false;   // WS_EX_TOOLWINDOW（タスクバーに出さない）

    bool Active() const { return mode != BackgroundMode::None; }
};

// --background の値部分（"=" の右。空文字 = 引数のみ）を解釈する。失敗なら false + err。
inline bool ParseBackgroundOption(std::string_view value, BackgroundOptions& out, std::string& err)
{
    BackgroundOptions opt;
    opt.mode = BackgroundMode::Offscreen;   // 既定
    bool toolExplicit = false;
    bool tool = false;

    size_t pos = 0;
    while (pos <= value.size())
    {
        const size_t comma = value.find(',', pos);
        std::string tok(value.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
        for (char& c : tok) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.pop_back();

        if (tok.empty()) {}
        else if (tok == "offscreen")  opt.mode = BackgroundMode::Offscreen;
        else if (tok == "minimized")  opt.mode = BackgroundMode::Minimized;
        else if (tok == "noactivate") opt.mode = BackgroundMode::NoActivate;
        else if (tok == "hidden")     opt.mode = BackgroundMode::Hidden;
        else if (tok == "tool")       { toolExplicit = true; tool = true; }
        else if (tok == "notool")     { toolExplicit = true; tool = false; }
        else
        {
            err = "unknown --background value '" + tok + "' (offscreen | minimized | noactivate | hidden, + tool | notool)";
            return false;
        }
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    // 画面外・非表示の窓にタスクバーボタンがあっても押されるだけで邪魔なので、既定で付ける。
    opt.toolWindow = toolExplicit ? tool
                                  : (opt.mode == BackgroundMode::Offscreen || opt.mode == BackgroundMode::Hidden);
    out = opt;
    return true;
}

inline const char* BackgroundModeName(BackgroundMode m)
{
    switch (m)
    {
    case BackgroundMode::Offscreen:  return "offscreen";
    case BackgroundMode::Minimized:  return "minimized";
    case BackgroundMode::NoActivate: return "noactivate";
    case BackgroundMode::Hidden:     return "hidden";
    default:                         return "none";
    }
}

// --background の論理解像度（クライアント領域）。最小化/画面外でも 0x0 にならない大きさを保つ。
constexpr unsigned kBackgroundClientWidth  = 1920;
constexpr unsigned kBackgroundClientHeight = 1080;

} // namespace dx12e
