#pragma once

namespace dx12e
{
// GitHub リリースによる自動アップデート（配布版のエディタ/ゲーム向け）。
//
// 仕組み:
//   1. 起動時（GPU 初期化より前）に GitHub の releases/latest を WinHTTP で取得。
//   2. タグ（vX.Y.Z）が現行 kEngineVersion より新しければ、更新するか確認ダイアログを出す。
//   3. 同意したらリリースの .zip アセットを %TEMP% にダウンロードして展開。
//   4. 本体終了を待って新ファイルを上書きし再起動する更新バッチを起動し、true を返す。
//      呼び出し側（main）は true なら即終了する（バッチが再起動を担う）。
//
// 配布レイアウト（exe 隣に assets/ がある）でのみ動作する。開発ビルドでは何もしない。
// ネットワーク失敗・リリース無し・タイムアウト時は静かに false を返して通常起動を続ける。
class Updater
{
public:
    // 起動時アップデートチェック。更新を開始して本体を終了すべきなら true。
    static bool RunStartupCheck();
};
} // namespace dx12e
