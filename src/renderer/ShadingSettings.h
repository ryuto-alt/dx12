#pragma once

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
};

} // namespace dx12e
