#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.5.5";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.5.5 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回直したこと:\n"
    "\n"
    "・ヒエラルキーで親オブジェクトにカメラをアタッチしても Play 中に追従しない不具合を修正。\n"
    "  親の移動・回転にカメラがちゃんとついていくように（ローカル変換ではなく\n"
    "  親階層込みのワールド変換でカメラを同期するように修正）。\n"
    "・エディタの回転ギズモを全面的に作り直し。\n"
    "  ワールド軸固定・無損失回転・弧ベースのマウス判定・視認性アップ。\n";
} // namespace dx12e
