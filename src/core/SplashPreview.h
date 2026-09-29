#pragma once

// ===========================================================================
// 起動画面のプレビュー（検証用）
//   DX12Engine.exe --splash-preview <出力ディレクトリ> [--dpi-scale N] [--splash-mode startup|project]
//                  [--splash-project <名前>] [--splash-backdrop mid|light|dark] [--splash-fps N]
// 窓を一切表示せず、オフスクリーンで【実窓と同じ描画コード】（SplashDirector + SplashRenderer）を
// 決定論的な時刻/進捗で走らせ、PNG 連番を書いて即終了する。音は出さない。エンジン本体（D3D12 デバイス・
// Application 等）は初期化しない軽量経路で、D3D11（無ければ WARP）だけを使う。
//   ・intro の各フレーム / loading 途中（進捗 10% / 40% / 70%）/ ready / transition の各段を出す
//   ・frames.csv に「番号, 時刻, フェーズ, %, ファイル名」を書く（コンタクトシートの見出し用）
// ===========================================================================

namespace dx12e
{

//
// もう 1 つの検証入口: DX12Engine.exe --splash-selftest <出力ディレクトリ> [--dpi-scale N]
//   実窓のコード（専用スレッド・60fps タイマ・D2D・UpdateLayeredWindow・Finish/Pump）を【窓を表示せずに】通し、
//   フレーム数 / 1 フレームの描画時間 / CPU 使用率 / メイン窓表示の呼び出し順を <dir>\selftest.txt に書く。音は鳴らさない。
//
// argv（Wide）に --splash-preview / --splash-selftest があれば実行して true を返し、exitCode に終了コードを入れる。
// 無ければ false（通常起動を続ける）。main の最初期（エンジン初期化より前）で呼ぶこと。
bool RunSplashPreviewIfRequested(int argc, wchar_t** argv, int& exitCode);

} // namespace dx12e
