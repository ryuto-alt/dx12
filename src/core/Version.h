#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.0.0";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.0.0 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.0.0: ゲーム内UI制作機能が一通り揃いました\n"
    "\n"
    "・UIエディタ(新設): 「窓 ▾ > UIエディタ」で開く専用2Dキャンバス(UMGデザイナー相当)。\n"
    "  ズーム/パン、画面サイズプレビュー(FHD/HD/縦/カスタム)、市松背景・グリッド表示。\n"
    "・UI編集: クリックでボタン丸ごと選択・ダブルクリックで中のラベルを選択(Figma方式)。\n"
    "  移動/8ハンドルリサイズ/アンカーハンドル直接編集、矢印キー移動、Del/Ctrl+D。\n"
    "・配置テンプレ: 中央/上下左右/四隅へワンクリック配置(アンカーも同時設定=解像度追従)。\n"
    "  Inspector の UIRect とUIエディタツールバーの両方から使えます。\n"
    "・UIAnimator(新コンポーネント): 出現アニメ(フェード/ポップ/スライド、イージング7種)、\n"
    "  ボタンのホバー/押下スケール、ループ(浮遊/パルス/点滅)をノーコード設定。\n"
    "・Lua: scene:tweenUi(イージング付きトゥイーン) / showUi / hideUi を追加。\n"
    "・UIImage の fill(HPバー/ゲージ)、テクスチャD&D割当も引き続き利用できます。\n";
} // namespace dx12e
