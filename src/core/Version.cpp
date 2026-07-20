#include "core/Version.h"

// バージョン定数の唯一の実体。宣言・注意書きは Version.h を参照。
namespace dx12e
{
const char* const kEngineVersion = "1.4.4";

const char* const kUpdateRepoOwner = "ryuto-alt";
const char* const kUpdateRepoName  = "dx12";

const char* const kWhatsNewTitle = "DX12 Engine v1.4.4 の更新内容";
const char* const kWhatsNewBody =
    "v1.4.4: エディタのクラッシュ対策 + 連番アニメ/UVスクロールの拡張\n"
    "\n"
    "・エディタ操作中のクラッシュ原因を多数修正\n"
    "  (壊れたシーンの親子ループ・プレハブ読込・削除後の選択残り・\n"
    "   スクリプト追加時の解析 など)\n"
    "・メッシュにも連番アニメ(スプライトシート)と UV スクロールが使えるように\n"
    "・UIImage の連番アニメ対応と animMode(ループ/単発/往復)追加\n"
    "・リファレンス/ドキュメントを更新\n"
    "\n"
    "※ v1.4.2 以前をお使いの場合、自動更新が失敗することがあります。\n"
    "   その場合は GitHub Releases から最新 zip を手動で入れ直してください。\n";
} // namespace dx12e
