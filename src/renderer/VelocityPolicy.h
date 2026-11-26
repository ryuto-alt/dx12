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

// PrepareFrame（描画リスト構築より前）が持っている入力。ここから理由を組むので、
// 「PrepareFrame の組み立て」自体をテストできる（理由の詰め漏れが今回の不具合の原因だった）。
struct PrepareFrameVelocityInputs
{
    bool taaActive        = false;   // taaCfg.enabled && m_taaPass ready && taaViewOk
    bool ssGiWantsPrepass = false;   // m_screenSpaceGi ready && taaViewOk && (SSR || SSGI 有効)
    bool ssrEnabled       = false;
    bool ssgiEnabled      = false;
    bool rtSceneReady     = false;   // m_rtScene がある（RenderView の useRt* の前提）
    bool taaPassExists    = false;   // m_taaPass がある（RT 影デノイザの前提）
    bool rtShadowEnabled  = false;
    bool rtShadowDenoise  = false;
    bool rtAoEnabled      = false;
    bool rtReflEnabled    = false;
};

// PrepareFrame の理由。RenderView の useRtAo / useRtShadowDenoise / useRtRefl と同じ前提（RT シーンの有無・m_taaPass の有無）に
// 揃える。taaViewOk（2D・正射）は掛けない: RenderView 側のデノイザ / RT-AO はそれを見ないので、掛けると 2D / 正射で
// velocityPrepass=true なのに前ワールドを追跡しない（物体の速度が 0 になる）フレームができる。
// stage（wiring.velocityReq）は PrepareFrame では分からないので false のまま（RenderView が true にしうる唯一の差）。
inline constexpr VelocityPrepassReasons MakePrepareFrameVelocityReasons(const PrepareFrameVelocityInputs& in) noexcept
{
    VelocityPrepassReasons r{};
    r.taa             = in.taaActive;
    r.ssr             = in.ssGiWantsPrepass && in.ssrEnabled;
    r.ssgi            = in.ssGiWantsPrepass && in.ssgiEnabled;
    r.rtShadowDenoise = in.rtSceneReady && in.taaPassExists && in.rtShadowEnabled && in.rtShadowDenoise;
    r.rtAo            = in.rtSceneReady && in.rtAoEnabled;
    r.rtReflections   = in.rtSceneReady && in.rtReflEnabled;
    return r;
}

// 前ワールドの追跡と前ワールド VB の確保が要るか。PrepareFrame の理由は velocityPrepass の上位集合に近いが、
// 完全な上位集合ではない: (a) stage だけが要求するフレームは PrepareFrame では分からない、
// (b) RenderView の useRtAo は screenSpaceOn も見る（PrepareFrame は見ない＝多めに払う側）。
// (a) のときは RenderDepthOnlyScene が VB を自分で確保する（面は欠けない）が、前ワールド追跡は無いので速度は 0 になる。
inline constexpr bool WantsVelocityPrevWorld(const VelocityPrepassReasons& r) noexcept
{
    return WantsVelocityPrepass(r);
}
} // namespace dx12e
