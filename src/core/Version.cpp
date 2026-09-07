#include "core/Version.h"

// バージョン定数の唯一の実体。宣言・注意書きは Version.h を参照。
namespace dx12e
{
const char* const    kEngineName  = "Uno Engine";
const wchar_t* const kEngineNameW = L"Uno Engine";

const char* const kEngineVersion = "1.14.0";

const char* const kUpdateRepoOwner = "ryuto-alt";
const char* const kUpdateRepoName  = "dx12";

const char* const kWhatsNewTitle = "Uno Engine v1.14.0 の更新内容";

const char* const kWhatsNewBody =
    "v1.14.0: 1 つのオブジェクトにエフェクトを重ねられるようになりました\n"
    "\n"
    "■【新機能】パーティクルを 1 つのオブジェクトに複数付けられます\n"
    "  これまでは 1 オブジェクトにつき放出器は 1 つだけでした。\n"
    "  松明なら「炎」「煙」「火の粉」を別々のオブジェクトに分けるしかなく、\n"
    "  動かすたびに全部を一緒に動かす必要がありました。\n"
    "\n"
    "  Inspector の Particle Emitter に「＋ レイヤーを追加」が付きました。\n"
    "  レイヤーごとに見た目・放出量・位置ずらし(Offset)を別々に設定できます。\n"
    "  パーティクルエディタからも「レイヤーとして追加」で重ねられます。\n"
    "\n"
    "    例) 松明 … 炎(先端) + 煙(その少し上) + 火の粉(たまに弾ける)\n"
    "\n"
    "■【新機能】イベントからレイヤーを名指しできます\n"
    "  Trigger の PlayEffect / StopEffect の str にレイヤー名を書くと、\n"
    "  そのレイヤーだけを鳴らす/止められます（空なら今までどおり全部）。\n"
    "  「煙は出しっぱなしで、火の粉だけ踏んだ瞬間に弾けさせる」ができます。\n"
    "\n"
    "■【新機能】パーティクル用のカスタムシェーダー\n"
    "  粒の見た目を自作の HLSL で差し替えられるようになりました。\n"
    "  レイヤー単位なので「炎は既定のまま、火の粉だけ自作」もできます。\n"
    "  雛形は create_shader の template=particle_ember から作れます。\n"
    "\n"
    "    ★メッシュ用とは別の契約です。#include \"UnoParticle.hlsli\" と\n"
    "      書けば、定数・ビルボード展開・ソフトパーティクルが揃います。\n"
    "      メッシュ用の雛形をそのまま貼っても動きません。\n"
    "\n"
    "■ 既存プロジェクトへの影響\n"
    "  ありません。今あるエフェクトは「レイヤー 1 枚」として読み込まれ、\n"
    "  見た目も保存されるデータも今までと同じままです。\n"
    "  （レイヤーが 1 枚のときは、シーンファイルの書き方も従来と同一です）\n";
} // namespace dx12e