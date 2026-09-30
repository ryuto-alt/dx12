#pragma once

// リファレンスレンダー窓（DXR パストレーサー = 地上真値レンダラ。パリティ基盤 Q1a）。
//
// - 独立フローティング窓（NavMeshPanel と同じ扱い。ドックしない）。
// - 入力・進捗は EditorContext::ptUi（pt::UiState の POD）越しに Application(core/ApplicationPathTracer.cpp)と受け渡す。
//   「開始」で requestStart を立て、Application が次のフレームで実行する。エンジンを固めないよう
//   1 フレームに使う GPU 時間は「フレーム予算」まで。
// - 出力は <保存先>.pfm(線形 HDR)/ .png(簡易プレビュー)/ .json(メタ)。仕様は docs/PATH_TRACER.md。
//
// 開き方: メニュー「ツール > リファレンスレンダー」。

namespace dx12e
{

class EditorContext;

namespace PathTracerPanel
{

// Application が毎フレーム呼ぶ唯一の入口（エディタモードのみ）。中で showPathTracer を見て早期 return する。
void Render(EditorContext& ctx);

} // namespace PathTracerPanel

} // namespace dx12e
