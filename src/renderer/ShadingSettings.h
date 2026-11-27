#pragma once

#include "core/Types.h"

namespace dx12e
{

// 材質シェーディングのシーン単位の設定（NormalFilterSettings と同じ流儀のヘッダオンリー定義。シリアライズ対象）。
//
// ▼ multiScatter（既定 ON）
//   GGX の単一散乱では、粗い面ほど「面の中で何度も跳ね返ってから出ていく光」が失われ、粗い金属が暗く沈む。
//   Kulla-Conty / Fdez-Agüera の近似で、方向アルベド E（split-sum の BRDF LUT の A + B）からスペキュラを
//   1 + F0·(1/E − 1) 倍に持ち上げ、拡散は残りのエネルギー 1 − E_ms だけにする（白い炉で 1 を超えない）。
//   直接光・IBL・反射プローブ・SSR / RT 反射（IBL の置き換えとして入る）の全経路で同じ係数。
//   OFF にすると従来（補償なし）と 1 ビットも変わらない絵になる。
//   GPU へは PerFrame の予約 _clusterReserved[9].x に「1 = 無効」で渡す（0 埋めの FrameConstants を使う
//   サムネイル等では有効＝シーンと同じ見た目）。
struct ShadingSettings
{
    bool multiScatter = true;

    // ▼ shadingRevision（W1-C1。80 TU から読まれる重ヘッダなので、ここで 1 回だけ足した）
    //   1 = 従来（V1。既定）。2 = UNO_SHADING_V2 の PSO / シェーダ（W1-C3 以降。マイクロシャドウ・GTSO・地平線・受光側の影の補正）。
    //   既存シーンは 1 のまま＝絵はビット一致。2 を選ぶのは新規シーンのルック（C8）と、品質プリセット Epic（C7）だけ。
    //   ★シーンへの保存 / 読み込みは scene/SceneShadingSerializerLaneC.cpp（W1-FU-E-27。既定と違う値だけ書く。rev は 1 / 2 のみ）。
    u8 shadingRevision = 1;

    // ▼ V2 の機能ごとの切り替え（C3 / C5 が読む。C1 では誰も読まない＝既定のままで絵は変わらない）。
    //   shadingRevision=2 かつ「全機能 OFF」の状態は V1 と同じ絵になること（C3 の受け入れ）。
    //   既定は C3 の設計どおり ON（V2 を選んだときに全部効く）。V1 のときは無視される。
    //   PfSlot::ShadingV2 の shadingFlags の microShadow / horizonOcc / gtso ビットに 1 対 1 で対応する。
    bool microShadow       = true;   // 面の微小な凹凸による自己影（microShadowK で強さ）
    bool horizonOcclusion  = true;   // 地平線オクルージョン（反射ベクトルが面の下へ潜る分を落とす。horizonK で強さ）
    bool gtso              = true;   // GTSO（AO から作る指向性スペキュラオクルージョン）

    u8 _pad[3] = {};

    f32 microShadowK = 1.0f;   // 0 .. 2（0 = 効果なし相当、1 = 標準）
    f32 horizonK     = 1.0f;   // 0 .. 2（同上）
};

} // namespace dx12e
