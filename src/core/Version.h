#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.9.8";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.9.8 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新: エディタUIをUE5風に一新 + ロード高速化\n"
    "\n"
    "・カスタムタイトルバー: OSの標準タイトルバーを廃止し、メニュー・シーン名・\n"
    "  ウィンドウ操作を統合したダークなタイトルバーに(Unreal/Unity風)。\n"
    "・FreeType フォント描画で文字がくっきり。Play/Stop ボタンは画面中央に。\n"
    "・multi-viewport: ツール窓(マテリアルエディタ等)をメインウィンドウの外へ\n"
    "  ドラッグすると独立したウィンドウになります(マルチモニタ向け)。\n"
    "・マテリアルサムネイル: プロジェクトロード時に事前生成し、結果をディスクに\n"
    "  キャッシュ。2回目以降のロードが大幅に高速化。ロード中はスプラッシュ表示で\n"
    "  固まらず、完了までエディタ画面を出しません。\n"
    "・修正: マテリアルD&Dが床グリッドに吸われる/回転オブジェクトに当たらない問題。\n"
    "・パーティクルに「向き Orient」を追加(ビルボード/水平/垂直)。\n";
} // namespace dx12e
