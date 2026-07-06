#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.9.5";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.9.5 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新: Xbox コントローラー対応(XInput) + 振動 + Lua Gamepad API\n"
    "\n"
    "・InputSystem が XInput 経由で最大4台のパッドをポーリング。ボタン/円形\n"
    "  デッドゾーン付きスティック/トリガー/振動(SetPadVibration・タイマー\n"
    "  自動停止付き SetPadVibrationTimed)を C++ で提供。\n"
    "・Lua 低レベル API: input:isPadButtonDown/Pressed/Released、\n"
    "  getPadLeftStickX/Y・getPadRightStickX/Y・getPadLeftTrigger/RightTrigger、\n"
    "  setPadVibration/setPadVibrationTimed。PAD_A/B/X/Y 等の定数も追加。\n"
    "・Lua 高レベル糖衣: padDown/padPressed/padReleased(名前文字列)、\n"
    "  padStick(\"left\"/\"right\")、padTrigger、padVibrate(low, high, sec?)。\n"
    "  keyDown/keyPressed と対称的な書き味で、pad 番号省略時は1台目。\n"
    "・docs/API_REFERENCE.md・SCRIPTING.md・使い方サイト・lua-defs 補完\n"
    "  スタブも同期済み。\n";
} // namespace dx12e
