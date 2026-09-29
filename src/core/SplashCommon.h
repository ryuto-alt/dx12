#pragma once

// スプラッシュの実窓（SplashScreen.cpp）とプレビュー（SplashPreview.cpp）が共有する小さな部品。

#include <string>
#include <string_view>
#include <vector>

namespace dx12e::splash
{

std::wstring Utf8ToWide(std::string_view s);

// Tips（SplashTips.h の kSplashTips[]）を wstring にしたもの。
std::vector<std::wstring> TipsWide();

// 「2026-09-30 12:34 ビルド」。exe の PE タイムスタンプ（リンク時刻）。決定論ビルド等で不正な値なら __DATE__/__TIME__ に切替。
std::wstring BuildInfoText();

// 最近開いたプロジェクトの名前（新しい順・最大 max 件）。%APPDATA%\DX12Engine\recent.json。無い/壊れていれば空。
// 存在しなくなったフォルダは除く。
std::vector<std::wstring> RecentProjectNames(int max);

// recent.json の中身（JSON 文字列）から名前を取り出す純関数版（テスト用）。
std::vector<std::wstring> ParseRecentNames(std::string_view json, int max);

// プライマリモニタ相当の表示倍率（Per-Monitor V2 の GetDpiForSystem / 96）。--dpi-scale の上書きも反映。
float EffectiveDpiScale();

} // namespace dx12e::splash
