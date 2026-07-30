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

    // ★ワーカースレッドの先頭で呼ぶ。
    //   SetUnhandledExceptionFilter はプロセス全体に効くが、
    //   **SetThreadStackGuarantee はスレッドごと**。Install() はメインスレッドでしか
    //   走らないので、これを呼ばないワーカーでスタックオーバーフローすると
    //   ハンドラを動かす余地が無く、**ログもダンプも残らずに落ちる**。
    //   実際に到達しうる: MaterialLibraryPanel はネットワークから取った JSON を
    //   ワーカーで再帰パースしている（nlohmann のパーサは再帰）。
    static void PrepareThread();

    // 「落ちる直前に何をしていたか」を残す。dx12_crash.log に新しい順で最大 16 件出る。
    // スタックトレースだけでは分からない“操作の文脈”(どのパネルのどの手順か)を補う。
    // クラッシュ処理中はヒープを触れないので、固定長リングへ strncpy するだけの実装。
    // 呼び出しコストはほぼゼロなので、時間のかかる処理の前に気軽に置いてよい。
    static void Breadcrumb(const char* text);
};

} // namespace dx12e
