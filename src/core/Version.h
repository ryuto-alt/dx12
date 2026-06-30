#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.6.2";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.6.2 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新（FPS の超安定化と MCP の不具合修正）:\n"
    "\n"
    "・シーン単位で影(CSM)の ON/OFF を切替可能に。影が不要なトップダウン等で\n"
    "  影パスを丸ごとスキップでき、敵が多いシーンの FPS が大きく改善。\n"
    "・毎フレームの GPU 全同期(WaitIdle)を撤去し、CPU と GPU を並列化。\n"
    "  フレーム時間が CPU+GPU から max(CPU,GPU) になり、高負荷時も安定。\n"
    "・フォワード描画に視錐台カリングを追加。画面外のオブジェクトを\n"
    "  描かずに省き、広いシーンほど効く。\n"
    "・MCP の不具合修正: ヘッドレス --build がポートファイルを死にポートで\n"
    "  上書きし、ライブの MCP ツールが切れる問題を解消（build はもうブリッジを\n"
    "  起動しない＋クライアントは再接続時にポートを再探索して自己回復）。\n";
} // namespace dx12e
