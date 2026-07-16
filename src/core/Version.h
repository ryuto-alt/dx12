#pragma once

// エンジンのバージョンと自動アップデート設定の単一ソース。
// リリースを公開するときは kEngineVersion を上げてからビルド/配布すること。
// 配布版（exe 隣に assets/ がある）は起動時に GitHub の最新リリースを確認し、
// kEngineVersion より新しいタグがあれば更新を促す（src/core/Updater.{h,cpp}）。
namespace dx12e
{
// セマンティックバージョン（"MAJOR.MINOR.PATCH"）。GitHub リリースのタグ（"v1.2.3" 等）と比較する。
constexpr const char* kEngineVersion = "1.1.0";

// 自動アップデートの取得元 GitHub リポジトリ。
constexpr const char* kUpdateRepoOwner = "ryuto-alt";
constexpr const char* kUpdateRepoName  = "dx12";

// 起動時に一度だけ表示する「更新内容」ポップアップ（この版で直したこと）。
// 表示済み判定は %LOCALAPPDATA%\DX12Engine\shown_version.txt（版が変わった初回だけ出す）。
// 新しい版を出すときは kEngineVersion を上げ、ここも書き換えること。
constexpr const char* kWhatsNewTitle = "DX12 Engine v1.1.0 の更新内容";
constexpr const char* kWhatsNewBody =
    "v1.1.0: ゲーム内UIの表現力を大幅拡張（形状/放射ゲージ/自動レイアウト/リッチテキスト）\n"
    "\n"
    "・UIImage に形状描画: 楕円/リング(円形ゲージ)/ダイヤ/六角形/三角形。テクスチャは形で\n"
    "  切り抜かれる(丸アイコン等)。放射fill(クールダウン円)・分割ゲージ(segments)・\n"
    "  タイルパターンの流し(uvScroll)・破線/コーナーブラケット枠・放射グラデも追加。\n"
    "・UIText: 字間(letterSpacing)・文字アニメ(ウェーブ/ジッター/レインボー)・本体グラデ・\n"
    "  リッチテキスト(rich=true で [c=RRGGBB]色[/c] [wave] [shake] [rainbow] タグ)。\n"
    "・UILayout コンポーネント新設(VBox/HBox/Grid): 子へセル矩形を自動配布=リストや\n"
    "  インベントリが offset 手計算なしで組める。UIRect.clipChildren で子のマスクも可能。\n"
    "・UIScrollView がドラッグ/フリック(慣性)スクロールに対応。ドラッグ中はリスト内\n"
    "  ボタンを誤クリックしない。\n"
    "・tweenUi 拡張: fill=(ゲージのなめらか増減)・countTo(数字ロール)・onComplete\n"
    "  (完了コールバック)・stopUiTweens(連打対策)。イージング5種追加(expo 等)。\n"
    "・uifx ワンライナー追加: stagger(順次入場)/damageBar(ゴーストバー)/countTo/heartbeat 等。\n"
    "・docs/UI_STYLE_GUIDE.md 新設: ジャンル別デザイン語彙→エンジン機能の対応表。\n";
} // namespace dx12e
