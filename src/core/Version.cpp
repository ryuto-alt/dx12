#include "core/Version.h"

// バージョン定数の唯一の実体。宣言・注意書きは Version.h を参照。
namespace dx12e
{
const char* const kEngineVersion = "1.4.5";

const char* const kUpdateRepoOwner = "ryuto-alt";
const char* const kUpdateRepoName  = "dx12";

const char* const kWhatsNewTitle = "DX12 Engine v1.4.5 の更新内容";
const char* const kWhatsNewBody =
    "v1.4.5: v1.4.4 で発生していたクラッシュ(ヒエラルキー選択/スクリプト・\n"
    "コンポーネント追加で落ちる)を修正\n"
    "\n"
    "・v1.4.4 の配布物に再コンパイル漏れの古いモジュールが混入し、\n"
    "  データ配置の不一致でエディタ操作時にクラッシュしていました\n"
    "・パッケージ作成時に再コンパイル漏れを検出するガードを追加(再発防止)\n"
    "\n"
    "v1.4.4 の内容(本バージョンに含まれます):\n"
    "・エディタ操作中のクラッシュ原因を多数修正\n"
    "・メッシュにも連番アニメ(スプライトシート)と UV スクロールが使えるように\n"
    "・UIImage の連番アニメ対応と animMode(ループ/単発/往復)追加\n"
    "\n"
    "※ v1.4.2 以前をお使いの場合、自動更新が失敗することがあります。\n"
    "   その場合は GitHub Releases から最新 zip を手動で入れ直してください。\n";
} // namespace dx12e
