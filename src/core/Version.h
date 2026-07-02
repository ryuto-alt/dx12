#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.6.3";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.6.3 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新（GitHub 連携を Unity/Unreal 並みに使いやすく）:\n"
    "\n"
    "・GitHub CLI(gh) をエディタに同梱。別途インストール不要で\n"
    "  「Git 変更」パネルからリポジトリ作成・ログインがそのまま使える。\n"
    "・空のプロジェクトでもログイン状態をすぐ表示。「初期化」の代わりに\n"
    "  「GitHub にリポジトリを作成 (Public/Private)」ボタン一発で\n"
    "  init→commit→作成→push まで完結。\n"
    "・ログイン完了をポーリングではなく即検知するように変更し、反映が速くなった。\n"
    "・作成するリポジトリ名を編集可能に（今までプロジェクト名固定だった）。\n"
    "・リポジトリのURLをパネルに常時表示し、クリックでブラウザを開けるように。\n"
    "・更新内容ポップアップを少し大きく見やすく。\n";
} // namespace dx12e
