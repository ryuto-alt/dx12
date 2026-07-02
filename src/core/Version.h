#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.8.4";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.8.4 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新（自動アップデートの信頼性向上・続き）:\n"
    "\n"
    "・更新の直後に再起動した時だけ更新確認を1回スキップしていた仕組みを\n"
    "  廃止しました。これからは更新した直後の起動を含め、毎回必ず更新を\n"
    "  確認します（スプラッシュ画面が表示されている間に確認するので、\n"
    "  体感の待ち時間はほぼ変わりません）。\n"
    "・パーティクルエディタ（ツール > パーティクルエディタ）を追加しました。\n"
    "  プレビューを見ながら色・サイズの変化をグラデーション/カーブで編集し、\n"
    "  名前付きで保存・配置済みエフェクトへの適用・Luaコード生成ができます。\n";
} // namespace dx12e
