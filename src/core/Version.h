#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "0.9.4";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v0.9.4 の更新内容";
constexpr const char* kWhatsNewBody =
    "今回の更新: time API 大幅拡張(ビデオ時計/個別時計/チャージ) + Lua 予測変換\n"
    "\n"
    "・time.video: ステージ共有の\"ビデオ時計\"を追加。ギミックの動きを\n"
    "  t = time.video.localTime(self) の純関数で書くと、video.skip(対象, ±秒) で\n"
    "  対象だけ先送り/巻き戻しできます。skip は残り時間を自動消費(skipCost)。\n"
    "・エンティティ個別時計: time.localTime/skipEntity/scaleEntity で\n"
    "  オブジェクト単位の停止(0)/スロー/逆再生(負)/スキップ。\n"
    "・charge: 押しっぱなしチャージ計測(弓を引く等)。ゲージ表示用の ratio()、\n"
    "  離した瞬間に量を返す released()。\n"
    "・コンソールの Lua 入力に予測変換を追加: time. や scene:fi まで打つと候補が\n"
    "  ポップアップ。Tab で確定・↑↓で選択・クリックで挿入。候補は実際の Lua\n"
    "  環境から動的列挙(自作グローバルも出ます)。\n";
} // namespace dx12e
