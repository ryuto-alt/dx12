#include "core/Version.h"

// バージョン定数の唯一の実体。宣言・注意書きは Version.h を参照。
namespace dx12e
{
const char* const kEngineVersion = "1.12.4";

const char* const kUpdateRepoOwner = "ryuto-alt";
const char* const kUpdateRepoName  = "dx12";

const char* const kWhatsNewTitle = "DX12 Engine v1.12.4 の更新内容";

const char* const kWhatsNewBody =
    "v1.12.4: シェーダーの演出を、コードを書かずにイベントで動かせるようになりました\n"
    "\n"
    "■【新機能】Trigger からシェーダーのパラメーターを動かせます\n"
    "  「部屋に入った瞬間まぶしくする」のような演出が、Lua を 1 行も書かずに\n"
    "  Inspector だけで組めるようになりました。Trigger のアクションに 2 つ追加しています。\n"
    "\n"
    "    SetShaderParam  … パラメーターへ値を即代入する\n"
    "    AnimShaderParam … パラメーターを「開始値 → 終了値」へ、指定した秒数で動かす\n"
    "\n"
    "  AnimShaderParam にはイージング（等速 / 減速 / 加速 / 両端ゆるめ）を選べます。\n"
    "  フラッシュが引いていく感じは「減速」が定番です。\n"
    "\n"
    "■ パラメーターは名前で選びます（打ち間違いが起きません）\n"
    "  アクションのパラメーター欄は、対象に割り当てられているシェーダーが実際に\n"
    "  宣言している名前だけが並ぶコンボです。手で名前を打つ必要がないので\n"
    "  「トリガーは動いているのに何も起きない」がそもそも起こりません。\n"
    "  名前が見つからないときは、使える名前の一覧をコンソールに出します。\n"
    "\n"
    "■【新機能】画面シェーダーにも名前付きパラメーターが使えます\n"
    "  v1.12.3 でメッシュに入れた「HLSL に書いた名前がそのまま Inspector に出る」仕組みを、\n"
    "  カメラの画面シェーダーにも広げました。ScreenShaderCB の params の位置に\n"
    "\n"
    "    float  _Flash;         // @range(0,1)\n"
    "    float3 _FlashColor;    // @color\n"
    "\n"
    "  のように書けば、その名前で Inspector に出て、そのまま Trigger から動かせます。\n"
    "  「新規シェーダー」で作られる画面シェーダーの雛形もこの形に変えました。\n"
    "\n"
    "■ 作り方（部屋に入ったらまぶしい）\n"
    "  1. 画面シェーダーを作って、上のように _Flash を宣言する\n"
    "  2. カメラの「画面シェーダー」に割り当てる\n"
    "  3. 部屋の入口に Trigger を置き、Enter に AnimShaderParam を足す\n"
    "     対象=カメラ / パラメーター=_Flash / 開始値=1 / 終了値=0 / 秒数=0.6 / 減速\n"
    "  詳しい手順は docs/AUTHORING.md の 2 章にあります。\n"
    "\n"
    "■ こまかい挙動\n"
    "  ・同じパラメーターに新しく発火すると、進行中のものは打ち切って掛け直します\n"
    "    （出入りを繰り返しても値が二重に動きません）\n"
    "  ・「居る間（Stay）」でも使えます。同じ指示のあいだは積み直さず走り続けます\n"
    "  ・対象を削除しても安全です。Play を止めると進行中の変化は破棄されます\n"
    "\n"
    "■ 既存シーンへの影響\n"
    "  ありません。シーンの保存形式は増えていません（既存の str / num / vec を使っています）。\n";
} // namespace dx12e
