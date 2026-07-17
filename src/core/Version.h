#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.1.1";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.1.1 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.1.1: モデル読み込み/ビルド出力の重要バグ修正 + MCP UI品質ツール\n"
    "\n"
    "・静的FBX/glTFのノード変換を頂点へ焼き込むように修正。Blender等でオブジェクトを\n"
    "  回転/移動して組んだモデルが崩れて表示される問題(矢が文字に刺さる等)を解消。\n"
    "・MCP spawn_model が assets 相対パスを解決できず全モデルのロードに失敗していたのを修正。\n"
    "・ビルドしたゲームの画面周囲に余白が出る問題を解消。\n"
    "・UICanvas に StretchToFill モード(scaleMode=2, レターボックス無し)を追加。\n"
    "・Box/Sphere/CapsuleCollider が Transform.scale を無視していたのを修正\n"
    "  (見た目だけ拡大した床が既定サイズでしか当たらない問題)。\n"
    "・MCP UI品質3点セット: dx12_ui_compare(2画面差分)/計測lint/dx12_install_font。\n";
} // namespace dx12e
