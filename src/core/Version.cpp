#include "core/Version.h"

// バージョン定数の唯一の実体。宣言・注意書きは Version.h を参照。
namespace dx12e
{
const char* const    kEngineName  = "Uno Engine";
const wchar_t* const kEngineNameW = L"Uno Engine";

const char* const kEngineVersion = "1.12.9";

const char* const kUpdateRepoOwner = "ryuto-alt";
const char* const kUpdateRepoName  = "dx12";

const char* const kWhatsNewTitle = "Uno Engine v1.12.9 の更新内容";

const char* const kWhatsNewBody =
    "v1.12.9: 当たり判定フィルタのチェックボックスが反応しなかったのを直しました\n"
    "\n"
    "■【修正】チェックボックスを押しても切り替わらない\n"
    "  v1.12.8 で足した「当たり判定の種類ごとの表示」ですが、色の見本と\n"
    "  チェックボックスが内部的に同じものとして扱われていて、クリックが\n"
    "  色の見本の方に取られていました。結果、どれも切り替えられませんでした。\n"
    "  フィルタを足したのに結局絞れない、という状態でした。申し訳ありません。\n"
    "\n"
    "  これで、色をクリックしてもチェックボックスをクリックしても切り替わります。\n"
    "\n"
    "■ 使い方（改めて）\n"
    "  設定 →「当たり判定を表示」を ON にすると、下に 2 種類の絞り込みが出ます。\n"
    "\n"
    "    表示範囲\n"
    "      ・選択中のみ（子も） … Hierarchy で選んだものとその子だけ\n"
    "        「このオブジェクトがすり抜ける」を調べるときはこれが一番速いです\n"
    "      ・カメラの近くだけ   … 距離をメートルで指定\n"
    "\n"
    "    種類ごとの表示（色の凡例がそのままチェックボックス）\n"
    "      ・線が埋まる原因はたいてい「動かない (Static)」（床と壁）です\n"
    "\n"
    "■ 既存プロジェクトへの影響\n"
    "  ありません。\n";
} // namespace dx12e
