#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.9.6";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.9.6 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新: マルチプレイヤー(オンライン対戦)機能 + クラッシュレポート\n"
    "\n"
    "・ネットワークシステム(ENet): Host/Join/切断、シーンベースライン同期、\n"
    "  spawn/despawn複製、スナップショット複製+補間、RPC、入力コマンド、\n"
    "  クライアント予測+サーバーリコンシリエーション、興味管理。\n"
    "・Lua API: net:host()/join()/disconnect()/isServer()/players()/\n"
    "  setInput{}/rpc 等。NetworkIdentity/NetworkTransform コンポーネント追加。\n"
    "・エディタ: ネットワーク設定窓(assets/network.json)と状態モニタ窓、\n"
    "  Playドロップダウンの「ホストとしてPlay」+「テストクライアント起動」\n"
    "  ボタンで1クリック2窓テスト(--net-client/--project CLI)。\n"
    "・クラッシュレポート: 予期しないクラッシュ時に dx12_crash.log(例外内容+\n"
    "  スタックトレース)と dx12_crash.dmp(ミニダンプ)を自動保存。原因調査が\n"
    "  できるようになりました。\n"
    "・Sprite2D のカスタム HLSL シェーダー対応、PointLight の castShadows。\n";
} // namespace dx12e
