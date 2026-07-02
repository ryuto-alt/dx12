#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.7.0";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.7.0 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新（HDR レンダリング + 大幅な安定化・高速化）:\n"
    "\n"
    "・HDR レンダリングパイプライン化。ブルームが光源や発光体の本来の\n"
    "  輝度から計算されるようになり、芯のあるグローに。トーンマップは\n"
    "  ポストプロセス最終段で一括適用（見た目が変わるためブルームの\n"
    "  強度・しきい値は再調整をおすすめします）。\n"
    "・ミップマップに対応。遠くのテクスチャのチラつき/ギラつきが解消。\n"
    "・GPU との同期を全面刷新。毎フレームの全停止をやめ、モデルや\n"
    "  テクスチャの読み込み中でもフレームが止まらなくなった。\n"
    "・安定性を大幅強化: 壊れたシーン JSON でアプリごと落ちない／\n"
    "  日本語フォルダのアセットが読めない問題を修正／GPU リソース枯渇を\n"
    "  即検知／フレーム内エラーからエディタへ確実に復帰。\n"
    "・Lua: スクリプトが OnStart/OnUpdate 未定義のとき game.lua の関数を\n"
    "  誤って呼ぶ問題を修正。hasComponent が全コンポーネント型に対応。\n"
    "・内部: コンポーネントの保存/復元を反射ベースに一元化（新規\n"
    "  コンポーネントの追加が実質2箇所で完結）。\n";
} // namespace dx12e
