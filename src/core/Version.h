#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.8.2";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.8.2 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新（VSCode で Lua スクリプトの予測変換）:\n"
    "\n"
    "・Lua API の型定義ファイルを同梱しました（tools\\lua-defs）。\n"
    "  VSCode の Lua 拡張（sumneko.lua）と組み合わせると、scene / fx /\n"
    "  actor など全 API の補完・説明表示・引数ヒント・打ち間違い検出が\n"
    "  効くようになります。導入手順は tools\\lua-defs\\README.md を参照。\n"
    "・hasComponent のコンポーネント名や fx の kind など、文字列引数の\n"
    "  中身まで候補が出ます。\n"
    "・使い方ドキュメント（Lua ガイド / API リファレンス）にコンソール\n"
    "  パネルと log / logWarn / logError の説明を追記しました。\n";
} // namespace dx12e
