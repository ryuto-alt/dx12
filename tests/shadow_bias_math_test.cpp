// Epic の動的 CSM（W1-C6 / LS-02a）の純関数（renderer/EpicCsm.h）と、シェーダ / C++ の配線の静的検査。GPU / DXC 不要。
// 実行: ctest --output-on-failure -R ShadowBiasMath

#include "renderer/EpicCsm.h"
#include "renderer/FrameConstants.h"
#include "renderer/ShadingV2Math.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef DX12E_SOURCE_ROOT
#error DX12E_SOURCE_ROOT が必要
#endif

using namespace dx12e;
namespace ec = dx12e::epiccsm;

namespace
{
int g_fail = 0, g_checks = 0;
#define CHECK_MSG(cond, ...)                                  \
    do {                                                      \
        ++g_checks;                                           \
        if (!(cond)) { ++g_fail; std::printf("  NG  "); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

std::string ReadFile(const std::string& rel)
{
    std::ifstream f(std::string(DX12E_SOURCE_ROOT) + "/" + rel, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    // CRLF を LF に揃える（検索を行末に依存させない）
    std::string out;
    out.reserve(s.size());
    for (char c : s) if (c != '\r') out.push_back(c);
    return out;
}

void Test_Layout()
{
    CHECK_MSG(ec::CascadeCount(false) == 4 && ec::CascadeCount(true) == 3, "旧 = 4、Epic = 3 カスケード");
    CHECK_MSG(ec::CascadeCount(false) == kFrameConstantsCascades, "旧経路の本数は FrameConstants の 4 と一致");
    CHECK_MSG(ec::CascadeFar(1000.0f, false) == 400.0f, "旧経路の遠端は 400m で頭打ち（従来値）");
    CHECK_MSG(ec::CascadeFar(1000.0f, true) == 200.0f && ec::CascadeFar(50.0f, true) == 50.0f, "Epic は 200m で頭打ち、近いカメラ遠端はそのまま");
    const auto o = ec::FormatsFor(false);
    CHECK_MSG(o.resource == DXGI_FORMAT_R32_TYPELESS && o.dsv == DXGI_FORMAT_D32_FLOAT && o.srv == DXGI_FORMAT_R32_FLOAT, "旧経路は従来の D32 の 3 点組");
    const auto e = ec::FormatsFor(true);
    CHECK_MSG(e.resource == DXGI_FORMAT_R16_TYPELESS && e.dsv == DXGI_FORMAT_D16_UNORM && e.srv == DXGI_FORMAT_R16_UNORM, "Epic は D16 の 3 点組");
    // VRAM: Epic（2048 D16 x3 = 24 MiB）は旧（2048 D32 x4 = 64 MiB）より小さい。増分は +24MB 以下。
    CHECK_MSG(ec::MapBytes(true, 2048) == 3ull * 2048 * 2048 * 2 && ec::MapBytes(true, 2048) <= 24ull * 1024 * 1024,
              "Epic の影マップは 24 MiB 以下: %llu", static_cast<unsigned long long>(ec::MapBytes(true, 2048)));
    CHECK_MSG(ec::MapBytes(false, 2048) == 64ull * 1024 * 1024, "旧は 64 MiB");
}

void Test_CasterBias()
{
    // 既定（旧経路 / V1）: 8000 / 2.0 のまま（ビット一致の根拠）
    const auto d32 = ec::CasterBiasFor(DXGI_FORMAT_D32_FLOAT, 1.0f);
    CHECK_MSG(d32.depthBias == 8000 && d32.slopeBias == 2.0f, "D32 scale=1 は従来の 8000 / 2.0");
    // D16: 同じ深度差（8000 * 2^-24）を D16 の単位（2^-16）で表す = 31.25 -> 31
    const auto d16 = ec::CasterBiasFor(DXGI_FORMAT_D16_UNORM, 1.0f);
    CHECK_MSG(d16.depthBias == 31 && d16.slopeBias == 2.0f, "D16 scale=1 は 31 / 2.0: %d", d16.depthBias);
    const double want = 8000.0 / 16777216.0;
    CHECK_MSG(std::fabs(d16.depthBias / 65535.0 - want) < 0.5 / 65535.0, "D16 のバイアスは D32 と同じ深度差（半単位以内）");
    // V2 の倍率で小さくなる（単調）。0 より大きい倍率なら最低 1 単位
    const auto q = ec::CasterBiasFor(DXGI_FORMAT_D16_UNORM, 0.25f);
    CHECK_MSG(q.depthBias < d16.depthBias && q.depthBias >= 1 && q.slopeBias == 0.5f, "scale=0.25 で小さくなる: %d %.2f", q.depthBias, q.slopeBias);
    CHECK_MSG(ec::CasterBiasFor(DXGI_FORMAT_D16_UNORM, 0.05f).depthBias >= 1, "最小倍率でも 0 にならない");
    CHECK_MSG(ec::CasterBiasFor(DXGI_FORMAT_D32_FLOAT, 0.25f).depthBias == 2000, "D32 でも倍率は素直に掛かる");
    // 倍率を掛けるのは「Epic かつ shadingRevision=2」だけ
    CHECK_MSG(ec::CasterBiasScale(false, 2, 0.25f) == 1.0f, "Epic でなければ V2 でも 1");
    CHECK_MSG(ec::CasterBiasScale(true, 1, 0.25f) == 1.0f, "Epic でも V1 のシーンは 1（V1 の受光面にアクネを出さない）");
    CHECK_MSG(ec::CasterBiasScale(true, 2, 0.25f) == 0.25f, "Epic かつ V2 は casterBiasScaleV2");
    CHECK_MSG(ec::CasterBiasScale(true, 2, 99.0f) == 1.0f && ec::CasterBiasScale(true, 2, 0.0f) == 0.05f, "倍率は 0.05..1 に丸める");
}

void Test_Pancake()
{
    CHECK_MSG(ec::PancakeDepth(-3.0f) == 0.0f && ec::PancakeDepth(0.4f) == 0.4f && ec::PancakeDepth(2.0f) == 1.0f, "深度クリップ OFF は [0,1] へ押し付ける");
    // near 面を外した視錐台: near の手前にある球も落とさない。それ以外の面（left/right/far）は従来どおり。
    using namespace DirectX;
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -10, 1), XMVectorSet(0, 0, 0, 1), XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixOrthographicOffCenterLH(-5, 5, -5, 5, 0.0f, 20.0f);
    Frustum f = Frustum::FromViewProj(view * proj);
    const XMVECTOR behindNear = XMVectorSet(0, 0, -15, 1);   // near（z=-10）より光側
    CHECK_MSG(!f.SphereVisible(behindNear, 1.0f), "従来の 6 面は near の手前の球を落とす");
    ec::DropNearPlane(f);
    CHECK_MSG(f.SphereVisible(behindNear, 1.0f), "near を外すと残る（pancaking で z=0 に潰れて影を落とす）");
    CHECK_MSG(!f.SphereVisible(XMVectorSet(50, 0, 0, 1), 1.0f), "left/right は従来どおり落とす");
    CHECK_MSG(!f.SphereVisible(XMVectorSet(0, 0, 40, 1), 1.0f), "far は従来どおり落とす");
    float planes[6][4] = {};
    planes[4][2] = 1.0f; planes[4][3] = -3.0f;
    ec::DropNearPlane(planes);
    CHECK_MSG(planes[4][0] == 0.0f && planes[4][1] == 0.0f && planes[4][2] == 0.0f && planes[4][3] == 1.0f, "配列版も常に内側の平面へ");
}

void Test_ReceiverOffset()
{
    CHECK_MSG(ec::ReceiverNormalOffset(0.02f, 1.5f, 2.0f, 1.0f) == 0.02f * 1.5f, "光に正対（N.L=1）は base だけ");
    CHECK_MSG(std::fabs(ec::ReceiverNormalOffset(0.02f, 1.5f, 2.0f, 0.0f) - 0.02f * 3.5f) < 1e-7f, "掠める角度（N.L=0）は base + slope");
    CHECK_MSG(ec::ReceiverNormalOffset(0.02f, 1.5f, 2.0f, -0.5f) == ec::ReceiverNormalOffset(0.02f, 1.5f, 2.0f, 0.5f), "裏面は |N.L|");
    CHECK_MSG(ec::ReceiverNormalOffset(0.0f, 1.5f, 2.0f, 0.3f) == 0.0f, "テクセル幅 0（使わないカスケード）はオフセット 0");
    CHECK_MSG(ec::ReceiverNormalOffset(0.1f, 1.5f, 2.0f, 0.5f) > ec::ReceiverNormalOffset(0.02f, 1.5f, 2.0f, 0.5f), "テクセルに比例");
}

void Test_Pack()
{
    const ShadowPcssSettings d;
    const auto bits = ec::PackReceiver(d.receiverNormalOffsetBase, d.receiverNormalOffsetSlope, d.gatherFilterTaps);
    const auto r = ec::UnpackReceiver(bits);
    CHECK_MSG(std::fabs(r.base - d.receiverNormalOffsetBase) < 4.0f / 255.0f && std::fabs(r.slope - d.receiverNormalOffsetSlope) < 4.0f / 255.0f
              && r.taps == d.gatherFilterTaps, "既定値の往復: %.3f %.3f %u", r.base, r.slope, r.taps);
    const auto big = ec::UnpackReceiver(ec::PackReceiver(99.0f, -1.0f, 1000u));
    CHECK_MSG(big.base == 4.0f && big.slope == 0.0f && big.taps == 32u, "範囲外は丸める（0..4 / 8..32）");
    CHECK_MSG(ec::UnpackReceiver(ec::PackReceiver(0.0f, 0.0f, 1u)).taps == 8u, "タップ数の下限は 8");
    const auto m = ec::UnusedCascadeMatrix();
    // 番兵の行列: どの world 点も x が範囲外（UV が [0,1] の外 = 影なし）
    const float x = 123.0f * m._11 + 45.0f * m._21 + 6.0f * m._31 + m._41;
    const float w = 123.0f * m._14 + 45.0f * m._24 + 6.0f * m._34 + m._44;
    CHECK_MSG(w == 1.0f && x / w * 0.5f + 0.5f > 1.0f, "使わないカスケードは必ず UV 範囲外");
}

void Test_Flags()
{
    ShadingSettings s;
    ShadowPcssSettings p;
    CHECK_MSG(shadingv2::PackFlags(s, p) == shadingv2::PackFlags(s), "既定（epicDynamicCsm=false）は従来のフラグと同じ");
    s.shadingRevision = 2;
    CHECK_MSG(shadingv2::PackFlags(s, p) == 7u, "V2 でも Epic でなければ追加ビットなし");
    p.epicDynamicCsm = true;
    CHECK_MSG(shadingv2::PackFlags(s, p) == (7u | shadingv2::kFlagCsmReceiver | shadingv2::kFlagCascadeDither), "Epic + V2 は受光側の法線オフセット（PCSS 無効なら Gather なし）");
    p.enabled = true;
    CHECK_MSG(shadingv2::PackFlags(s, p) == (7u | shadingv2::kFlagCsmReceiver | shadingv2::kFlagCascadeDither | shadingv2::kFlagGatherPcss), "PCSS 有効なら Gather も");
    p.gatherPcss = false;
    CHECK_MSG((shadingv2::PackFlags(s, p) & shadingv2::kFlagGatherPcss) == 0u, "gatherPcss=false は Gather なし");
    p.cascadeDitherBlend = false;
    CHECK_MSG((shadingv2::PackFlags(s, p) & shadingv2::kFlagCascadeDither) == 0u, "cascadeDitherBlend=false はディザなし");
    p.cascadeDitherBlend = true;
    s.shadingRevision = 1;
    CHECK_MSG(shadingv2::PackFlags(s, p) == 0u, "V1 のシーンは Epic の設定が立っていても x = 0（V1 と同じ絵）");
}

void Test_Wiring()
{
    const std::string pcss = ReadFile("shaders/forward/ShadowPcss.hlsli");
    const std::string shade = ReadFile("shaders/forward/ForwardShade.hlsli");
    const std::string light = ReadFile("shaders/forward/Lighting.hlsli");
    // V1 を変えない: Gather 版は #ifdef UNO_SHADING_V2 の内側だけ
    const size_t gather = pcss.find("float SampleShadowPcssGather");
    const size_t v2open = pcss.rfind("#ifdef UNO_SHADING_V2", gather);
    const size_t v2close = pcss.find("#endif // UNO_SHADING_V2", gather);
    CHECK_MSG(gather != std::string::npos && v2open != std::string::npos && v2close != std::string::npos && v2open < gather && gather < v2close,
              "Gather の PCSS は UNO_SHADING_V2 の内側だけ");
    const size_t calc = shade.find("float CalcShadowV2");
    const size_t sopen = shade.rfind("#ifdef UNO_SHADING_V2", calc);
    const size_t sclose = shade.find("#endif // UNO_SHADING_V2", calc);
    CHECK_MSG(calc != std::string::npos && sopen < calc && calc < sclose, "CalcShadowV2 は UNO_SHADING_V2 の内側だけ");
    CHECK_MSG(shade.find("#else\n    float shadow = CalcShadow(si.worldPos, si.viewDepth, si.svPos);\n#endif") != std::string::npos,
              "V1 の経路は従来の CalcShadow の 1 行のまま");
    CHECK_MSG(light.find("UNO_V2_FLAG_CSM_RECEIVER = 1u << 3") != std::string::npos && light.find("UNO_V2_FLAG_GATHER_PCSS  = 1u << 4") != std::string::npos,
              "HLSL のフラグのビットが C++ と同じ（3 / 4）");
    CHECK_MSG(shadingv2::kFlagCsmReceiver == (1u << 3) && shadingv2::kFlagGatherPcss == (1u << 4) && shadingv2::kFlagCascadeDither == (1u << 5), "C++ のフラグのビット");
    CHECK_MSG(light.find("UNO_V2_FLAG_CASCADE_DITHER = 1u << 5") != std::string::npos, "HLSL のディザのビットが C++ と同じ（5）");
    CHECK_MSG(shade.find("(rb & 255u) * (4.0 / 255.0)") != std::string::npos && shade.find("((rb >> 8) & 255u) * (4.0 / 255.0)") != std::string::npos
              && shade.find("(rb >> 16) & 255u") != std::string::npos, "受光側の係数のアンパックが C++ の UnpackReceiver と同じ");
    // CPU: pancaking のカリングは影のカスケードだけ（旧経路・スポット・ポイント・プリパスは 6 面のまま）
    const std::string render = ReadFile("src/core/ApplicationRender.cpp");
    CHECK_MSG(render.find("shadowPass && cascadeTexelWorld > 0.0f && m_backendExt && m_backendExt->epicCsm.active") != std::string::npos,
              "RenderDepthOnlyScene の near 面を外すのは Epic の CSM のカスケードだけ");
    CHECK_MSG(ReadFile("src/core/ApplicationVg.cpp").find("epiccsm::DropNearPlane(fp.p)") != std::string::npos, "VG の影の錐台からも near 面を外す");
    CHECK_MSG(ReadFile("src/renderer/vg/VirtualGeometrySystem.cpp").find("sv.dropNearPlane") != std::string::npos, "VG の GPU カリングの平面からも near 面を外す");
    CHECK_MSG(ReadFile("src/renderer/foliage/FoliageSystem.cpp").find("dropNearPlane") != std::string::npos, "植生の影のカリングからも near 面を外す");
}
} // namespace

int main()
{
    Test_Layout();
    Test_CasterBias();
    Test_Pancake();
    Test_ReceiverOffset();
    Test_Pack();
    Test_Flags();
    Test_Wiring();
    std::printf("shadow_bias_math_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
