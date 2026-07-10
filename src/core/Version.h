#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.0.1";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.0.1 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.0.1: 選択ラベルの改善 + メッシュ用カスタムシェーダー値\n"
    "\n"
    "・選択中オブジェクトのラベルをシーンビューの邪魔にならない位置へ移動\n"
    "  (Unreal Engine方式、エディタウィンドウ左下に常時表示)。\n"
    "・MeshRenderer に effectValue を追加(Sprite2D と同じくカスタムシェーダーへ渡す\n"
    "  汎用の進捗/強度値)。Lua scene:setMeshEffect(entity, value) で実行時に書き換え可能。\n"
    "・カスタムシェーダーのコンパイル失敗時、直前まで動いていたバイトコードを維持するよう修正\n"
    "  (一時的な保存エラーで見た目が壊れるのを防止)。\n"
    "・HRESULT 例外メッセージにエラーコード(16進)を追記し、DirectX エラーの原因調査をしやすく。\n";
} // namespace dx12e
