#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.2.0";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.2.0 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.2.0: モデル差し替え機能 + エディタ操作の重要バグ修正\n"
    "\n"
    "・モデル差し替え機能: Inspector の MeshRenderer に「モデル」欄を追加。assets 内の\n"
    "  モデル一覧から選択、またはアセットブラウザからドロップするだけで差し替え完了。\n"
    "  Transform/スクリプト/物理などの設定と親子関係はそのまま維持。Ctrl+Z で戻せる。\n"
    "・回転したモデルをクリックしても選択できない問題を修正(判定が回転を無視していた)。\n"
    "・Play→Stop でモデルのサイズ/形状が編集時と変わることがある問題を修正\n"
    "  (モデルキャッシュのパス表記ゆれで Stop のたびにディスクから再ロードしていた)。\n"
    "・アセットブラウザでシーンをダブルクリックするとクラッシュすることがある問題を修正。\n";
} // namespace dx12e
