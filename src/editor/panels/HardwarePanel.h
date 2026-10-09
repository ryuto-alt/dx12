#pragma once

// ハードウェア窓（Arduino / ESP32 の接続状態と一発書き込み）。
//
// - 独立フローティング窓（PathTracerPanel と同じ扱い。ドックしない）。メニュー「ツール > ハードウェア」。
// - 入力・状態は EditorContext::hwUi（hwui::UiState）越しに Application(core/mcp/ApplicationMcpHardware.cpp の
//   ServiceHardwareUi)と受け渡す。パネルは要求（requestFlash / requestToolchainInstall）と選択だけ書き、
//   スナップショットを読む。書き込みはボタンを押したときだけ（自動では書かない）。
// - ボードが見つかったのに hello が来ないときは Application が showHardware を立てて案内文（promptMessage）を入れる。

namespace dx12e
{

class EditorContext;

namespace HardwarePanel
{

// Application が毎フレーム呼ぶ唯一の入口（エディタモードのみ）。中で showHardware を見て早期 return する。
void Render(EditorContext& ctx);

} // namespace HardwarePanel

} // namespace dx12e
