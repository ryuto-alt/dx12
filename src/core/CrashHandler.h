#pragma once

namespace dx12e
{

// 予期しないクラッシュ(アクセス違反等のSEH例外 / std::terminate / abort / 純粋仮想呼び出し)を
// 捕まえて、CWD(= dx12_engine.log と同じ場所)へ以下を書き出す:
//   - dx12_crash.log : 人が読めるレポート(例外コード/アドレス/全スレッドではなく当該スレッドの
//                      スタックトレース。PDB があれば関数名+行番号、無ければ モジュール+オフセット)
//   - dx12_crash.dmp : ミニダンプ(Visual Studio / WinDbg で開いて事後解析できる)
// 直前の実行ログは dx12_engine.log に残る(Logger が毎メッセージ即時フラッシュするため)。
class CrashHandler
{
public:
    // WinMain の先頭で1回呼ぶ(Logger より前でよい。ハンドラはログシステムに依存しない)。
    static void Install();
};

} // namespace dx12e
