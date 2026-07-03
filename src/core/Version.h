#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.9.0";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.9.0 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新: プロジェクト独自シェーダー + 保存で即ホットリロード\n"
    "\n"
    "・assets/shaders/ に .hlsl を置くと、保存した瞬間にゲーム/シーンビューへ\n"
    "  自動反映されるようになりました（0.5秒ポーリング → 実行時コンパイル →\n"
    "  差し替え。エディタの再起動もビルドも不要）。全35シェーダー(Forward/\n"
    "  ポスト処理/パーティクル/IBL/SSAO等の全パス)が対象です。\n"
    "・既存シェーダーと同じ相対パスに置けば「上書き」、別パスなら「自作」として\n"
    "  扱われ、Inspectorの MeshRenderer「Shader」欄から個別メッシュに割り当て\n"
    "  られます（静的メッシュ対応）。ツールバー「ファイル > 新規シェーダー」や\n"
    "  アセットブラウザ右クリックからテンプレートを作成し、VS Code で編集できます。\n"
    "・コンパイルエラー時は直前のシェーダーを保持したままコンソールに赤字で\n"
    "  表示されるので、壊れた状態のまま止まりません。\n"
    "・配布ビルド(--build)でもプロジェクトシェーダーがコンパイルされて\n"
    "  ゲームパッケージに反映されます（壊れたシェーダーがあればビルド自体を\n"
    "  中止し、古い版を誤って出荷しません）。\n";
} // namespace dx12e
