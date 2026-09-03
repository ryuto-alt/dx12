#include "core/Version.h"

// バージョン定数の唯一の実体。宣言・注意書きは Version.h を参照。
namespace dx12e
{
const char* const kEngineVersion = "1.12.3";

const char* const kUpdateRepoOwner = "ryuto-alt";
const char* const kUpdateRepoName  = "dx12";

const char* const kWhatsNewTitle = "DX12 Engine v1.12.3 の更新内容";

const char* const kWhatsNewBody =
    "v1.12.3: カスタムシェーダーのパラメーターに名前を付けられるようになりました\n"
    "\n"
    "■【新機能】HLSL に書いた名前が、そのまま Inspector に出ます\n"
    "  今までカスタムシェーダーへ渡せる値は「エフェクト値」と「パラメーター」の 2 行だけで、\n"
    "  横に並んだ 4 つのスライダーのどれが何なのかは、書いた本人しか分かりませんでした。\n"
    "  これからは cbuffer に書いた変数の名前が、そのまま項目名になります。\n"
    "\n"
    "    float  _Glow;        // @range(0,4)\n"
    "    float3 _TintColor;   // @color\n"
    "\n"
    "  この 2 行を足して保存するだけで、Inspector の Shader 欄に「_Glow」のスライダーと\n"
    "  「_TintColor」のカラーピッカーが生えます。\n"
    "  ★保存するとホットリロードで項目が増えます。エディタの再起動は要りません。\n"
    "\n"
    "■ 行末に書ける注釈\n"
    "  ・// @range(min,max) … ドラッグではなくスライダーになります\n"
    "  ・// @color          … float3 / float4 がカラーピッカーになります\n"
    "    （名前に Color や Tint が入っていれば、注釈なしでも自動で色扱いです）\n"
    "  ・pad / reserved / dummy などの名前は詰め物とみなして表示しません\n"
    "\n"
    "■【改善】使える枠が 5 個から 8 個に増えました\n"
    "  今まで詰め物として 0 で埋めていた 3 つ分（旧 _pad）も、パラメーターとして使える\n"
    "  ようにしました。float / float2 / float3 / float4 で合計 8 個まで置けます\n"
    "  （この 8 個がエンジンの定数の予算いっぱいなので、これ以上は増やせません）。\n"
    "\n"
    "■ 使える型\n"
    "  float / float2 / float3 / float4 のみです。int・bool・行列・配列は項目としては\n"
    "  出ますが編集できません（値の入れ物が float だからです）。\n"
    "\n"
    "■ 既存シーンへの影響\n"
    "  ありません。今までのシェーダーは今までどおり「エフェクト値 / パラメーター」の 2 行で\n"
    "  表示され、保存済みの値もそのまま読み込まれます。\n"
    "  シェーダーの情報が読めない状況でも自動的に従来の 2 行へ切り替わるので、\n"
    "  割り当てが外れることはありません。\n";
} // namespace dx12e
