#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.9.7";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.9.7 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新: マテリアル関連の使い勝手を大幅改善\n"
    "\n"
    "・アセットブラウザ: .dxmat マテリアルを球体プレビューで表示するように\n"
    "  なりました(ライティング込みの見た目で一覧できます)。\n"
    "・マテリアルの D&D 適用: アセットブラウザから .dxmat をシーンビューの\n"
    "  オブジェクトへ直接ドロップして適用できます(サブメッシュ単位、\n"
    "  Undo対応。Unity/Unreal と同じ操作感)。\n"
    "・マテリアルライブラリ: Poly Haven の API 変更でダウンロードが\n"
    "  失敗していた問題を修正しました。\n"
    "・マテリアルエディタ: ウィンドウの初期サイズを拡大、プレビュー球の\n"
    "  ドラッグ回転の向きを反転して直感的にしました。\n";
} // namespace dx12e
