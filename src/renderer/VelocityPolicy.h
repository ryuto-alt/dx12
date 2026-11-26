#pragma once
// 速度 + G-Buffer プリパス（PrepassMode::DepthVelocityGBuffer）を走らせる理由と、それに伴う前提の純ロジック。
//
// ★FU-E-23（NB3 のパリティで見つかった「RT 影 ON でラスタの大きな地面・塔の上部が欠ける」）の原因:
//   RT 影のデノイザだけが速度プリパスを要求するフレーム（TAA / SSR / SSGI が全部 OFF）では、
//   「前ワールド行列の VB（EnsureInstancePrevBuffer）」が確保されておらず、RenderDepthOnlyScene が
//   速度パスの自動インスタンシングを諦めて個別ドロー（CPU で world * viewProj を畳んだ MVP）へ落ちていた。
//   フォワードは同じ物をインスタンシング（mul(world, pos) → mul(wp, viewProj) の 2 段）で描くので、
//   クリップ Z がビット一致せず、LESS_EQUAL で大きな面（遠い地面・高い塔）が欠けた（z-fight の縞）。
//   → 速度パスを要求する理由が 1 つでもあれば、前ワールド VB の確保と前ワールドの追跡を同じ条件で行う。
// ここに理由を足すときは、必ず ApplicationRender.cpp の velocityPrepass の条件と揃えること。

namespace dx12e
{
struct VelocityPrepassReasons
{
    bool taa              = false;   // TAA / DLSS
    bool ssr              = false;   // SSR
    bool ssgi             = false;   // SSGI
    bool rtShadowDenoise  = false;   // RT 影デノイザ（RtSettings::shadowEnabled && shadowDenoise）
    bool rtAo             = false;   // RT-AO
    bool rtReflections    = false;   // RT 反射
};

// 速度パスが走り得るか（前ワールドの追跡と前ワールド VB の確保が要るか）。
inline constexpr bool WantsVelocityPrevWorld(const VelocityPrepassReasons& r) noexcept
{
    return r.taa || r.ssr || r.ssgi || r.rtShadowDenoise || r.rtAo || r.rtReflections;
}
} // namespace dx12e
