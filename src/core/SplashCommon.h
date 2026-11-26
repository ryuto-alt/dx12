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

// 指定モニター（HMONITOR）の表示倍率（GetDpiForMonitor / 96。--dpi-scale の上書きは EffectiveDpiScale と同じに反映）。取れなければ EffectiveDpiScale()。
float MonitorDpiScale(void* hmonitor);

// ---- 起動画面の見た目（W1-BD3）
// コマンドライン文字列（GetCommandLineW の全体）と環境変数 UNO_SPLASH_STYLE の値から、Graphite Gauge を使うか決める純関数。
// 「--splash-style classic」（または環境変数 classic）なら false（従来のカード）。それ以外（無指定・gauge）は true。
// コマンドラインが環境変数より優先。
bool ParseGaugeStyle(std::wstring_view commandLine, std::wstring_view envValue);
// 実プロセスの値で上を呼ぶ。
bool UseGaugeStyle();
// Reduce Motion（SPI_GETCLIENTAREAANIMATION が FALSE）。UNO_SPLASH_REDUCE_MOTION=1 でも有効（検証用）。
bool ReduceMotionRequested();
// 起動画面の書体（Uno Splash Mono）のパス。assetsRoot = <assets>。無ければ空。
std::wstring SplashFontPath(const std::wstring& assetsRoot);

} // namespace dx12e::splash
