// シーン設定（shading / shadowPcss）の拡張欄の保存・読み込み（W1-FU-E-27、lane-C サイドカー）。
#include "scene/SceneShadingSerializerLaneC.h"

#include "scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace dx12e
{
using json = nlohmann::json;

void SaveSceneShadingLaneC(const Scene& scene, json& root)
{
    // ---- shading: shadingRevision と V2 の欄 ----
    {
        const ShadingSettings& sh = scene.GetShadingSettings();
        static const ShadingSettings kDef{};
        json add = json::object();
        if (sh.shadingRevision != kDef.shadingRevision)     add["shadingRevision"] = static_cast<int>(sh.shadingRevision);
        if (sh.microShadow != kDef.microShadow)             add["microShadow"]     = sh.microShadow;
        if (sh.horizonOcclusion != kDef.horizonOcclusion)   add["horizonOcclusion"] = sh.horizonOcclusion;
        if (sh.gtso != kDef.gtso)                           add["gtso"]            = sh.gtso;
        if (sh.microShadowK != kDef.microShadowK)           add["microShadowK"]    = sh.microShadowK;
        if (sh.horizonK != kDef.horizonK)                   add["horizonK"]        = sh.horizonK;
        if (!add.empty())
        {
            json& sj = root["shading"];
            if (!sj.is_object()) sj = json::object();
            for (auto it = add.begin(); it != add.end(); ++it) sj[it.key()] = it.value();
        }
    }

    // ---- shadowPcss: LS-02a（Epic の太陽影）の欄 ----
    {
        const ShadowPcssSettings& pc = scene.GetShadowPcssSettings();
        static const ShadowPcssSettings kDef{};
        json add = json::object();
        if (pc.epicDynamicCsm != kDef.epicDynamicCsm)         add["epicDynamicCsm"]            = pc.epicDynamicCsm;
        if (pc.pancaking != kDef.pancaking)                   add["pancaking"]                 = pc.pancaking;
        if (pc.gatherPcss != kDef.gatherPcss)                 add["gatherPcss"]                = pc.gatherPcss;
        if (pc.cascadeDitherBlend != kDef.cascadeDitherBlend) add["cascadeDitherBlend"]        = pc.cascadeDitherBlend;
        if (pc.receiverNormalOffsetBase != kDef.receiverNormalOffsetBase)   add["receiverNormalOffsetBase"]  = pc.receiverNormalOffsetBase;
        if (pc.receiverNormalOffsetSlope != kDef.receiverNormalOffsetSlope) add["receiverNormalOffsetSlope"] = pc.receiverNormalOffsetSlope;
        if (pc.casterBiasScaleV2 != kDef.casterBiasScaleV2)   add["casterBiasScaleV2"]         = pc.casterBiasScaleV2;
        if (pc.gatherFilterTaps != kDef.gatherFilterTaps)     add["gatherFilterTaps"]          = pc.gatherFilterTaps;
        if (!add.empty())
        {
            json& pj = root["shadowPcss"];
            if (!pj.is_object()) pj = json::object();
            for (auto it = add.begin(); it != add.end(); ++it) pj[it.key()] = it.value();
        }
    }
}

void LoadSceneShadingLaneC(Scene& scene, const json& root)
{
    try
    {
        if (root.contains("shading") && root["shading"].is_object())
        {
            const auto& shj = root["shading"];
            ShadingSettings& sh = scene.GetShadingSettings();
            sh.shadingRevision   = (shj.value("shadingRevision", 1) == 2) ? 2 : 1;
            sh.microShadow       = shj.value("microShadow",      sh.microShadow);
            sh.horizonOcclusion  = shj.value("horizonOcclusion", sh.horizonOcclusion);
            sh.gtso              = shj.value("gtso",             sh.gtso);
            sh.microShadowK      = std::clamp(shj.value("microShadowK", sh.microShadowK), 0.0f, 2.0f);
            sh.horizonK          = std::clamp(shj.value("horizonK",     sh.horizonK),     0.0f, 2.0f);
        }
    }
    catch (const json::exception&) { /* 型不正は既定のまま（本体の shading 読み込みと同じ扱い） */ }

    try
    {
        if (root.contains("shadowPcss") && root["shadowPcss"].is_object())
        {
            const auto& pj = root["shadowPcss"];
            ShadowPcssSettings& pc = scene.GetShadowPcssSettings();
            pc.epicDynamicCsm      = pj.value("epicDynamicCsm",      pc.epicDynamicCsm);
            pc.pancaking           = pj.value("pancaking",           pc.pancaking);
            pc.gatherPcss          = pj.value("gatherPcss",          pc.gatherPcss);
            pc.cascadeDitherBlend  = pj.value("cascadeDitherBlend",  pc.cascadeDitherBlend);
            pc.receiverNormalOffsetBase  = std::clamp(pj.value("receiverNormalOffsetBase",  pc.receiverNormalOffsetBase),  0.0f, 4.0f);
            pc.receiverNormalOffsetSlope = std::clamp(pj.value("receiverNormalOffsetSlope", pc.receiverNormalOffsetSlope), 0.0f, 4.0f);
            pc.casterBiasScaleV2   = std::clamp(pj.value("casterBiasScaleV2", pc.casterBiasScaleV2), 0.05f, 1.0f);
            pc.gatherFilterTaps    = std::clamp<u32>(pj.value("gatherFilterTaps", pc.gatherFilterTaps), 8u, 32u);
        }
    }
    catch (const json::exception&) { }
}
} // namespace dx12e
