#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.8.5";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.8.5 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新:\n"
    "\n"
    "・ワールドスプライトの回転がギズモで効かない不具合を修正しました。新規に\n"
    "  配置したスプライトは既定でビルボード(常にカメラ正対)がOFFになり、\n"
    "  Transformの回転がそのまま反映されます。Inspectorにスプライト専用の\n"
    "  設定項目（テクスチャ/サイズ/UV/色/ビルボードON-OFF等）を追加しました。\n"
    "・Gitが見つからない環境向けに、Gitパネルから「Gitをインストール」ボタンで\n"
    "  導入できるようにしました（winget が使えれば自動、無ければ公式ダウンロード\n"
    "  ページを開きます）。\n"
    "・プロジェクトを削除しなくても、ファイルメニューまたはウィンドウの×から\n"
    "  プロジェクト選択画面に戻れるようになりました。\n";
} // namespace dx12e
