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
// 理由を足すときは WantsVelocityPrepass だけを直す（呼び出し側は両方ともこの関数を使う）。

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
    bool stage            = false;   // Stage（wiring.velocityReq）。PrepareFrame の時点では分からないので false のまま渡す
};

// 速度プリパスを走らせる理由が 1 つでもあるか。ApplicationRender.cpp の PrepareFrame（前ワールド追跡 / VB 確保）と
// velocityPrepass の両方がこの関数で決める（条件を 2 か所に書かない）。
inline constexpr bool WantsVelocityPrepass(const VelocityPrepassReasons& r) noexcept
{
    return r.taa || r.ssr || r.ssgi || r.rtShadowDenoise || r.rtAo || r.rtReflections || r.stage;
}

// 前ワールドの追跡と前ワールド VB の確保が要るか。PrepareFrame の時点の理由は velocityPrepass の部分集合ではなく上位集合
// （RT が未準備・screenSpaceOn が false などで実際には走らないフレームでも true になりうる）。そのぶん前ワールドの
// emplace と VB（37MB）を払うが、速度パスが走るフレームで確保漏れが起きないことを優先する。
inline constexpr bool WantsVelocityPrevWorld(const VelocityPrepassReasons& r) noexcept
{
    return WantsVelocityPrepass(r);
}
} // namespace dx12e
