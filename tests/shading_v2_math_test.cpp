// UNO_SHADING_V2 のマイクロシャドウ / GTSO / 地平線オクルージョンの CPU 写し（renderer/ShadingV2Math.h）の性質と、
// シェーダ（shaders/forward/Lighting.hlsli・ForwardShade.hlsli）との配線の静的検査。GPU / DXC 不要。W1-C5。
// 実行: ctest --output-on-failure -R ShadingV2Math

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
namespace sv2 = dx12e::shadingv2;

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
    return ss.str();
}

void Test_Flags()
{
    ShadingSettings s;
    CHECK_MSG(sv2::PackFlags(s) == 0u, "shadingRevision=1 は全機能 OFF（V2 の PSO が残っていても V1 と同じ絵）");
    CHECK_MSG(std::bit_cast<std::uint32_t>(sv2::PackSlot(s).x) == 0u, "revision=1 の x は 0");
    s.shadingRevision = 2;
    CHECK_MSG(sv2::PackFlags(s) == 7u, "revision=2 の既定は 3 機能とも ON: %u", sv2::PackFlags(s));
    s.gtso = false;
    CHECK_MSG(sv2::PackFlags(s) == (sv2::kFlagMicroShadow | sv2::kFlagHorizonOcc), "gtso だけ OFF");
    s.microShadow = false; s.horizonOcclusion = false;
    CHECK_MSG(sv2::PackFlags(s) == 0u, "全機能 OFF の revision=2 は x=0（V1 と同じ絵）");
    s.microShadowK = 5.0f; s.horizonK = -1.0f;
    const auto v = sv2::PackSlot(s);
    CHECK_MSG(v.y == 2.0f && v.z == 0.0f, "強さは 0..2 に丸める: %.2f %.2f", v.y, v.z);
    s.microShadowK = NAN;
    CHECK_MSG(sv2::PackSlot(s).y == 0.0f, "NaN は 0");
}

void Test_MicroShadow()
{
    CHECK_MSG(sv2::MicroShadow(0.7f, 1.0f, 1.0f) == 1.0f, "ao=1 は遮蔽なし");
    CHECK_MSG(sv2::MicroShadow(0.7f, 0.0f, 1.0f) < 0.31f, "ao=0 は N.L=0.7 で 0.3 まで落ちる: %.3f", sv2::MicroShadow(0.7f, 0.0f, 1.0f));
    CHECK_MSG(sv2::MicroShadow(1.0f, 0.0f, 1.0f) == 0.0f, "ao=0（完全に塞がれた）は N.L=1 でも 0");
    CHECK_MSG(std::fabs(sv2::MicroShadow(1.0f, 0.5f, 1.0f) - 0.5f) < 1e-6f, "ao=0.5・N.L=1 は 2 ao^2 = 0.5");
    for (float ao : {0.0f, 0.3f, 0.7f})
        CHECK_MSG(sv2::MicroShadow(0.5f, ao, 0.0f) == 1.0f, "k=0 は恒等 ao=%.1f", ao);
    float prev = -1.0f;
    for (float ao = 0.0f; ao <= 1.0f; ao += 0.1f)
    {
        const float m = sv2::MicroShadow(0.4f, ao, 1.0f);
        CHECK_MSG(m >= prev - 1e-6f, "ao について単調増加 ao=%.1f", ao);
        prev = m;
    }
    CHECK_MSG(sv2::MicroShadow(0.4f, 0.4f, 2.0f) <= sv2::MicroShadow(0.4f, 0.4f, 1.0f), "k が大きいほど暗い");
    CHECK_MSG(sv2::MicroShadow(-0.6f, 0.2f, 1.0f) == sv2::MicroShadow(0.6f, 0.2f, 1.0f), "|N.L|");
}

void Test_Horizon()
{
    CHECK_MSG(sv2::HorizonOcclusion(0.3f, 1.0f) == 1.0f, "面の上を向く反射は 1");
    CHECK_MSG(sv2::HorizonOcclusion(-1.0f, 1.0f) == 0.0f, "真下へ向く反射は 0");
    CHECK_MSG(std::fabs(sv2::HorizonOcclusion(-0.5f, 1.0f) - 0.25f) < 1e-6f, "(1-0.5)^2 = 0.25");
    CHECK_MSG(sv2::HorizonOcclusion(-0.5f, 0.0f) == 1.0f, "k=0 は恒等");
    CHECK_MSG(sv2::HorizonOcclusion(-0.3f, 2.0f) < sv2::HorizonOcclusion(-0.3f, 1.0f), "k が大きいほど落ちる");
}

void Test_Gtso()
{
    // 遮蔽なし（ao=1）は、どの粗さ・どの向きでも 1（掠める角度のフレネルの反射を落とさない）
    for (float r : {0.05f, 0.2f, 0.5f, 0.8f, 1.0f})
        for (float noR : {1.0f, 0.7f, 0.3f, 0.0f, -0.2f})
            CHECK_MSG(sv2::Gtso(noR, 1.0f, r) > 0.9999f, "ao=1 は 1 r=%.2f NoR=%.1f: %.4f", r, noR, sv2::Gtso(noR, 1.0f, r));
    // 範囲と単調性
    for (float r : {0.1f, 0.4f, 0.8f})
        for (float noR : {0.9f, 0.5f, 0.1f})
        {
            float prev = -1.0f;
            for (float ao = 0.0f; ao <= 1.0f; ao += 0.05f)
            {
                const float so = sv2::Gtso(noR, ao, r);
                CHECK_MSG(so >= 0.0f && so <= 1.0f, "0..1 r=%.1f NoR=%.1f ao=%.2f: %.4f", r, noR, ao, so);
                CHECK_MSG(so >= prev - 1e-4f, "ao について単調増加 r=%.1f NoR=%.1f ao=%.2f: %.4f < %.4f", r, noR, ao, so, prev);
                prev = so;
            }
        }
    // 反射が可視円錐（N 中心）の中に収まれば 1（凹部でも真正面を向く細い反射は遮られない）
    CHECK_MSG(sv2::Gtso(1.0f, 0.1f, 0.1f) > 0.999f, "N と同じ向きの細い反射は可視円錐の中: %.4f", sv2::Gtso(1.0f, 0.1f, 0.1f));
    // 滑らかな面（細いローブ）が、凹部の可視円錐の外へ向くと真っ暗、ローブが広い（粗い）と残る
    CHECK_MSG(sv2::Gtso(0.2f, 0.1f, 0.1f) < 0.05f, "滑らかな面の凹部の縁へ向く反射は暗い: %.4f", sv2::Gtso(0.2f, 0.1f, 0.1f));
    CHECK_MSG(sv2::Gtso(0.2f, 0.1f, 0.9f) > sv2::Gtso(0.2f, 0.1f, 0.1f) + 0.02f, "粗い面は残る: %.4f", sv2::Gtso(0.2f, 0.1f, 0.9f));
    // 掠める角度（NoR -> 0）でも、ao が中程度なら V1 の Lagarde（約 ao）から大きく落ちない（黒い縁の回帰）
    for (float r : {0.1f, 0.3f, 0.6f})
        CHECK_MSG(sv2::Gtso(0.0f, 0.7f, r) > 0.5f, "掠める角度で黒い縁が出ない r=%.1f: %.4f", r, sv2::Gtso(0.0f, 0.7f, r));
    // 反射が可視円錐の外へ向く（NoR が小さい）ほど暗い
    CHECK_MSG(sv2::Gtso(0.2f, 0.3f, 0.3f) < sv2::Gtso(1.0f, 0.3f, 0.3f), "法線から離れた反射は暗い");
}

void Test_ShaderWiring()
{
    const std::string lighting = ReadFile("shaders/forward/Lighting.hlsli");
    const std::string shade    = ReadFile("shaders/forward/ForwardShade.hlsli");
    CHECK_MSG(lighting.find("UNO_V2_FLAG_MICRO_SHADOW = 1u << 0") != std::string::npos, "HLSL のフラグ 0 = マイクロシャドウ");
    CHECK_MSG(lighting.find("UNO_V2_FLAG_HORIZON_OCC  = 1u << 1") != std::string::npos, "HLSL のフラグ 1 = 地平線");
    CHECK_MSG(lighting.find("UNO_V2_FLAG_GTSO         = 1u << 2") != std::string::npos, "HLSL のフラグ 2 = GTSO");
    CHECK_MSG(sv2::kFlagMicroShadow == 1u && sv2::kFlagHorizonOcc == 2u && sv2::kFlagGtso == 4u, "C++ 側のビット");
    CHECK_MSG(lighting.find("_clusterReserved[2].x") != std::string::npos, "HLSL は PfSlot::ShadingV2（2 番）を読む");
    CHECK_MSG(static_cast<unsigned>(PfSlot::ShadingV2) == 2u, "PfSlot::ShadingV2 == 2");
    // V1 を守る: V2 の記述は #ifdef UNO_SHADING_V2 の中だけ（V2 の文字列の直前に開きがある）
    for (const std::string* src : {&lighting, &shade})
    {
        std::size_t pos = 0;
        while ((pos = src->find("UnoV2", pos)) != std::string::npos)
        {
            const std::size_t open = src->rfind("#ifdef UNO_SHADING_V2", pos);
            // 行頭（コメントを除く）の UnoV2 の使用が、直近の #ifdef UNO_SHADING_V2 より後ろにあることだけ見る
            CHECK_MSG(open != std::string::npos && open < pos, "UnoV2* の使用が #ifdef UNO_SHADING_V2 より前にある");
            pos += 5;
        }
    }
}
} // namespace

int main()
{
    Test_Flags();
    Test_MicroShadow();
    Test_Horizon();
    Test_Gtso();
    Test_ShaderWiring();
    std::printf("ShadingV2Math: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
