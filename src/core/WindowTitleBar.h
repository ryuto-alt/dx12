#pragma once

// Windows 11 のダークタイトルバー（W1-A7 / MOD-ED-19）。実体は Window.cpp。Window.h（60 TU）は変えない。
//   DwmSetWindowAttribute を dwmapi.dll から動的に引く。古い Windows（属性が無い）や失敗は黙って無視する。
//   dark = true: DWMWA_USE_IMMERSIVE_DARK_MODE（20）と、キャプション色（35）・枠線色（34）・文字色（36）を captionRgb / textRgb へ、
//              システムのバックドロップ（38）を無しにする。dark = false: 既定（DWMWA_COLOR_DEFAULT）へ戻す。
//   色は 0xRRGGBB。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace dx12e
{
void ApplyTitleBarTheme(HWND hwnd, bool dark, unsigned captionRgb, unsigned textRgb);
} // namespace dx12e
