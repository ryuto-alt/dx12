#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.0.2";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.0.2 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.0.2: UIエディタの階層ツリー + カスタムシェーダーのパラメーター調整\n"
    "\n"
    "・UIエディタに階層ツリー(左ペイン)を追加。ドラッグ&ドロップで UI 要素の親変更、\n"
    "  上下端へのドロップで兄弟の並べ替え(=描画順の変更。上=奥/下=手前)ができる。\n"
    "  並び順は UIRect::order としてシーンに保存され、クリックの前面判定にも効く。\n"
    "・カスタムシェーダーへ渡せる汎用パラメーター float4 shaderParams を MeshRenderer と\n"
    "  Sprite2D に追加。Inspector の Shader セクションの4連スライダーで調整でき、\n"
    "  Lua scene:setMeshParams / scene:setSpriteParams(entity, x,y,z,w) で実行時にも変更可能。\n"
    "  HLSL側: メッシュは cbuffer の effectValue の後ろに float4 shaderParams; を追加、\n"
    "  スプライトは VSIn/PSIn に float4 params : TEXCOORD2; を追加(docs/AUTHORING.md 参照)。\n"
    "・エディタ終了時に MaterialPreviewRenderer の解放順の問題でクラッシュする不具合を修正。\n";
} // namespace dx12e
