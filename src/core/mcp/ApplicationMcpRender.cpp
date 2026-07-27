// ===========================================================================
// MCP: 描画設定（ポスト / SSAO / SSR / SSGI / TAA / フォグ / DXR）
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// method の足し方は本ファイル内 McpDefine の並びに倣う（作法は ApplicationInternal.h の DX12E_MCP_HANDLER 付近）。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{
using namespace appdetail;


// ---- 描画設定（ポスト / SSAO / SSR / SSGI / コンタクトシャドウ / TAA / フォグ） ----
void Application::RegisterMcpRenderMethods()
{
    using json = nlohmann::json;
    namespace fs = std::filesystem;

    // ════════════════════════════════════════════════════════════
    //  ビジュアル/ポスト設定の操作(ポストプロセス・SSAO)
    // ════════════════════════════════════════════════════════════
    McpDefine("get_post_process", "", DX12E_MCP_HANDLER
        {
            const auto& pp = m_scene->GetPostSettings();
            resp["ok"] = true;
            resp["result"] = {
                {"enabled", pp.enabled},
                {"exposureOn", pp.exposureOn}, {"exposure", pp.exposure},
                {"contrastOn", pp.contrastOn}, {"contrast", pp.contrast},
                {"brightnessOn", pp.brightnessOn}, {"brightness", pp.brightness},
                {"saturationOn", pp.saturationOn}, {"saturation", pp.saturation},
                {"warmthOn", pp.warmthOn}, {"warmth", pp.warmth},
                {"hueOn", pp.hueOn}, {"hueShift", pp.hueShift},
                {"tintOn", pp.tintOn}, {"tint", {pp.tint.x, pp.tint.y, pp.tint.z}},
                {"bloomOn", pp.bloomOn}, {"bloom", pp.bloom}, {"bloomThreshold", pp.bloomThreshold},
                {"vignetteOn", pp.vignetteOn}, {"vignette", pp.vignette},
                {"chromaticOn", pp.chromaticOn}, {"chromatic", pp.chromatic},
                {"pixelizeOn", pp.pixelizeOn}, {"pixelSize", pp.pixelSize},
                {"posterizeOn", pp.posterizeOn}, {"posterize", pp.posterize},
                {"ditherOn", pp.ditherOn}, {"ditherLevels", pp.ditherLevels},
                {"scanlineOn", pp.scanlineOn}, {"scanline", pp.scanline},
                {"sharpenOn", pp.sharpenOn}, {"sharpen", pp.sharpen},
                {"grainOn", pp.grainOn}, {"grain", pp.grain},
                {"invertOn", pp.invertOn}, {"invert", pp.invert},
                {"sepiaOn", pp.sepiaOn}, {"sepia", pp.sepia},
                {"grayscaleOn", pp.grayscaleOn}, {"grayscale", pp.grayscale},
                {"lensOn", pp.lensOn}, {"lens", pp.lens},
                {"waveOn", pp.waveOn}, {"waveAmp", pp.waveAmp}, {"waveFreq", pp.waveFreq}, {"waveSpeed", pp.waveSpeed},
                {"radialOn", pp.radialOn}, {"radial", pp.radial},
                {"glitchOn", pp.glitchOn}, {"glitch", pp.glitch},
                {"outlineOn", pp.outlineOn}, {"outline", pp.outline},
                {"outlineColor", {pp.outlineColor.x, pp.outlineColor.y, pp.outlineColor.z}},
                {"fxaaOn", pp.fxaaOn},
                {"tonemapper", pp.tonemapper},
                {"bloomKnee", pp.bloomKnee}, {"bloomRadius", pp.bloomRadius},
                {"autoExposureOn", pp.autoExposureOn}, {"aeSpeed", pp.aeSpeed}, {"aeEvComp", pp.aeEvComp},
                {"aeLogMin", pp.aeLogMin}, {"aeLogMax", pp.aeLogMax},
                {"lutOn", pp.lutOn}, {"lutPath", pp.lutPath}, {"lutAmount", pp.lutAmount},
                {"debandOn", pp.debandOn},
                {"godraysOn", pp.godraysOn}, {"grIntensity", pp.grIntensity},
                {"grDensity", pp.grDensity}, {"grDecay", pp.grDecay},
                {"lensflareOn", pp.lensflareOn}, {"lfIntensity", pp.lfIntensity},
                {"lfGhosts", pp.lfGhosts}, {"lfDispersal", pp.lfDispersal},
                {"lfHalo", pp.lfHalo}, {"lfChroma", pp.lfChroma},
                {"dofOn", pp.dofOn}, {"dofFocusDist", pp.dofFocusDist},
                {"dofFocusRange", pp.dofFocusRange}, {"dofBlurSize", pp.dofBlurSize},
                {"motionBlurOn", pp.motionBlurOn}, {"mbStrength", pp.mbStrength},
                {"mbSamples", pp.mbSamples},
            };
        });

    McpDefine("set_post_process", McpPostParamSpec(), DX12E_MCP_HANDLER
        {
            auto& pp = m_scene->GetPostSettings();
            pp.enabled = params.value("enabled", pp.enabled);
            pp.exposureOn = params.value("exposureOn", pp.exposureOn); pp.exposure = params.value("exposure", pp.exposure);
            pp.contrastOn = params.value("contrastOn", pp.contrastOn); pp.contrast = params.value("contrast", pp.contrast);
            pp.brightnessOn = params.value("brightnessOn", pp.brightnessOn); pp.brightness = params.value("brightness", pp.brightness);
            pp.saturationOn = params.value("saturationOn", pp.saturationOn); pp.saturation = params.value("saturation", pp.saturation);
            pp.warmthOn = params.value("warmthOn", pp.warmthOn); pp.warmth = params.value("warmth", pp.warmth);
            pp.hueOn = params.value("hueOn", pp.hueOn); pp.hueShift = params.value("hueShift", pp.hueShift);
            pp.tintOn = params.value("tintOn", pp.tintOn);
            if (params.contains("tint"))
            {
                auto t = params["tint"].get<std::vector<float>>();
                if (t.size() == 3) pp.tint = {t[0], t[1], t[2]};
            }
            pp.bloomOn = params.value("bloomOn", pp.bloomOn); pp.bloom = params.value("bloom", pp.bloom);
            pp.bloomThreshold = params.value("bloomThreshold", pp.bloomThreshold);
            pp.bloomKnee = params.value("bloomKnee", pp.bloomKnee); pp.bloomRadius = params.value("bloomRadius", pp.bloomRadius);
            pp.tonemapper = params.value("tonemapper", pp.tonemapper);
            pp.autoExposureOn = params.value("autoExposureOn", pp.autoExposureOn);
            pp.aeSpeed = params.value("aeSpeed", pp.aeSpeed); pp.aeEvComp = params.value("aeEvComp", pp.aeEvComp);
            pp.aeLogMin = params.value("aeLogMin", pp.aeLogMin); pp.aeLogMax = params.value("aeLogMax", pp.aeLogMax);
            pp.lutOn = params.value("lutOn", pp.lutOn); pp.lutPath = params.value("lutPath", pp.lutPath);
            pp.lutAmount = params.value("lutAmount", pp.lutAmount);
            pp.debandOn = params.value("debandOn", pp.debandOn);
            pp.godraysOn = params.value("godraysOn", pp.godraysOn);
            pp.grIntensity = params.value("grIntensity", pp.grIntensity);
            pp.grDensity = params.value("grDensity", pp.grDensity);
            pp.grDecay = params.value("grDecay", pp.grDecay);
            pp.lensflareOn = params.value("lensflareOn", pp.lensflareOn);
            pp.lfIntensity = params.value("lfIntensity", pp.lfIntensity);
            pp.lfGhosts = params.value("lfGhosts", pp.lfGhosts);
            pp.lfDispersal = params.value("lfDispersal", pp.lfDispersal);
            pp.lfHalo = params.value("lfHalo", pp.lfHalo);
            pp.lfChroma = params.value("lfChroma", pp.lfChroma);
            pp.dofOn = params.value("dofOn", pp.dofOn);
            pp.dofFocusDist = params.value("dofFocusDist", pp.dofFocusDist);
            pp.dofFocusRange = params.value("dofFocusRange", pp.dofFocusRange);
            pp.dofBlurSize = params.value("dofBlurSize", pp.dofBlurSize);
            pp.motionBlurOn = params.value("motionBlurOn", pp.motionBlurOn);
            pp.mbStrength = params.value("mbStrength", pp.mbStrength);
            pp.mbSamples = params.value("mbSamples", pp.mbSamples);
            pp.vignetteOn = params.value("vignetteOn", pp.vignetteOn); pp.vignette = params.value("vignette", pp.vignette);
            pp.chromaticOn = params.value("chromaticOn", pp.chromaticOn); pp.chromatic = params.value("chromatic", pp.chromatic);
            pp.pixelizeOn = params.value("pixelizeOn", pp.pixelizeOn); pp.pixelSize = params.value("pixelSize", pp.pixelSize);
            pp.posterizeOn = params.value("posterizeOn", pp.posterizeOn); pp.posterize = params.value("posterize", pp.posterize);
            pp.ditherOn = params.value("ditherOn", pp.ditherOn); pp.ditherLevels = params.value("ditherLevels", pp.ditherLevels);
            pp.scanlineOn = params.value("scanlineOn", pp.scanlineOn); pp.scanline = params.value("scanline", pp.scanline);
            pp.sharpenOn = params.value("sharpenOn", pp.sharpenOn); pp.sharpen = params.value("sharpen", pp.sharpen);
            pp.grainOn = params.value("grainOn", pp.grainOn); pp.grain = params.value("grain", pp.grain);
            pp.invertOn = params.value("invertOn", pp.invertOn); pp.invert = params.value("invert", pp.invert);
            pp.sepiaOn = params.value("sepiaOn", pp.sepiaOn); pp.sepia = params.value("sepia", pp.sepia);
            pp.grayscaleOn = params.value("grayscaleOn", pp.grayscaleOn); pp.grayscale = params.value("grayscale", pp.grayscale);
            pp.lensOn = params.value("lensOn", pp.lensOn); pp.lens = params.value("lens", pp.lens);
            pp.waveOn = params.value("waveOn", pp.waveOn); pp.waveAmp = params.value("waveAmp", pp.waveAmp);
            pp.waveFreq = params.value("waveFreq", pp.waveFreq); pp.waveSpeed = params.value("waveSpeed", pp.waveSpeed);
            pp.radialOn = params.value("radialOn", pp.radialOn); pp.radial = params.value("radial", pp.radial);
            pp.glitchOn = params.value("glitchOn", pp.glitchOn); pp.glitch = params.value("glitch", pp.glitch);
            pp.outlineOn = params.value("outlineOn", pp.outlineOn); pp.outline = params.value("outline", pp.outline);
            if (params.contains("outlineColor"))
            {
                auto c = params["outlineColor"].get<std::vector<float>>();
                if (c.size() == 3) pp.outlineColor = {c[0], c[1], c[2]};
            }
            pp.fxaaOn = params.value("fxaaOn", pp.fxaaOn);
            resp["ok"] = true;
            resp["result"] = {{"applied", true}};
        });

    McpDefine("get_ssao", "", DX12E_MCP_HANDLER
        {
            const auto& s = m_scene->GetSSAOSettings();
            resp["ok"] = true;
            resp["result"] = {{"enabled", s.enabled}, {"radius", s.radius}, {"bias", s.bias},
                              {"intensity", s.intensity}, {"power", s.power},
                              {"sampleCount", s.sampleCount}, {"blur", s.blur}};
        });

    McpDefine("set_ssao", McpSsaoParamSpec(), DX12E_MCP_HANDLER
        {
            auto& s = m_scene->GetSSAOSettings();
            s.enabled = params.value("enabled", s.enabled);
            s.radius = params.value("radius", s.radius);
            s.bias = params.value("bias", s.bias);
            s.intensity = params.value("intensity", s.intensity);
            s.power = params.value("power", s.power);
            s.sampleCount = params.value("sampleCount", s.sampleCount);
            s.blur = params.value("blur", s.blur);
            resp["ok"] = true;
            resp["result"] = {{"applied", true}};
        });

    McpDefine("get_ssr", "", DX12E_MCP_HANDLER
        {
            const auto& s = m_scene->GetSsrSettings();
            resp["ok"] = true;
            resp["result"] = {{"enabled", s.enabled}, {"intensity", s.intensity},
                              {"maxDistance", s.maxDistance}, {"thickness", s.thickness},
                              {"maxSteps", s.maxSteps}, {"stride", s.stride},
                              {"roughnessCutoff", s.roughnessCutoff}, {"edgeFade", s.edgeFade},
                              {"bias", s.bias}};
        });

    McpDefine("set_ssr", "bias:any,edgeFade:any,enabled:any,intensity:any,maxDistance:any,maxSteps:any,"
              "roughnessCutoff:any,stride:any,thickness:any", DX12E_MCP_HANDLER
        {
            auto& s = m_scene->GetSsrSettings();
            s.enabled         = params.value("enabled",         s.enabled);
            s.intensity       = params.value("intensity",       s.intensity);
            s.maxDistance     = params.value("maxDistance",     s.maxDistance);
            s.thickness       = params.value("thickness",       s.thickness);
            s.maxSteps        = params.value("maxSteps",        s.maxSteps);
            s.stride          = params.value("stride",          s.stride);
            s.roughnessCutoff = params.value("roughnessCutoff", s.roughnessCutoff);
            s.edgeFade        = params.value("edgeFade",        s.edgeFade);
            s.bias            = params.value("bias",            s.bias);
            resp["ok"] = true;
            resp["result"] = {{"applied", true}};
        });

    McpDefine("get_ssgi", "", DX12E_MCP_HANDLER
        {
            const auto& s = m_scene->GetSsgiSettings();
            resp["ok"] = true;
            resp["result"] = {{"enabled", s.enabled}, {"intensity", s.intensity},
                              {"radius", s.radius}, {"thickness", s.thickness},
                              {"rayCount", s.rayCount}, {"stepCount", s.stepCount},
                              {"clampValue", s.clampValue}, {"feedback", s.feedback},
                              {"iblFallback", s.iblFallback}};
        });

    McpDefine("set_ssgi", "clampValue:any,enabled:any,feedback:any,iblFallback:any,intensity:any,radius:any,"
              "rayCount:any,stepCount:any,thickness:any", DX12E_MCP_HANDLER
        {
            auto& s = m_scene->GetSsgiSettings();
            s.enabled     = params.value("enabled",     s.enabled);
            s.intensity   = params.value("intensity",   s.intensity);
            s.radius      = params.value("radius",      s.radius);
            s.thickness   = params.value("thickness",   s.thickness);
            s.rayCount    = params.value("rayCount",    s.rayCount);
            s.stepCount   = params.value("stepCount",   s.stepCount);
            s.clampValue  = params.value("clampValue",  s.clampValue);
            s.feedback    = params.value("feedback",    s.feedback);
            s.iblFallback = params.value("iblFallback", s.iblFallback);
            resp["ok"] = true;
            resp["result"] = {{"applied", true}};
        });

    McpDefine("get_contact_shadow", "", DX12E_MCP_HANDLER
        {
            const auto& c = m_scene->GetContactShadowSettings();
            resp["ok"] = true;
            resp["result"] = {{"enabled", c.enabled}, {"rayLength", c.rayLength},
                              {"thickness", c.thickness}, {"bias", c.bias},
                              {"intensity", c.intensity}, {"steps", c.steps},
                              {"maxDistance", c.maxDistance}, {"fadeDistance", c.fadeDistance}};
        });

    McpDefine("set_contact_shadow", "bias:any,enabled:any,fadeDistance:any,intensity:any,maxDistance:any,rayLength:any,"
              "steps:any,thickness:any", DX12E_MCP_HANDLER
        {
            auto& c = m_scene->GetContactShadowSettings();
            c.enabled      = params.value("enabled",      c.enabled);
            c.rayLength    = params.value("rayLength",    c.rayLength);
            c.thickness    = params.value("thickness",    c.thickness);
            c.bias         = params.value("bias",         c.bias);
            c.intensity    = params.value("intensity",    c.intensity);
            c.steps        = params.value("steps",        c.steps);
            c.maxDistance  = params.value("maxDistance",  c.maxDistance);
            c.fadeDistance = params.value("fadeDistance", c.fadeDistance);
            resp["ok"] = true;
            resp["result"] = {{"applied", true}};
        });

    // ---- 内部解像度スケール（#16。レンダー解像度と表示解像度の分離）----
    // 3D シーンだけを scale 倍の解像度で描き、最終パスで表示解像度へ引き伸ばす。
    // UI / ImGui / エディタのギズモは常に表示解像度のまま＝文字がボケない。
    McpDefine("get_render_scale|set_render_scale", "scale:number", DX12E_MCP_HANDLER
        {
            if (method == "set_render_scale")
            {
                if (!params.contains("scale"))
                    throw McpError(McpErr::InvalidParam, "need scale (0.25..1.0)");
                SetRenderScale(static_cast<f32>(McpFloatParam(params, "scale", 1.0f, 0.25f, 1.0f)));
            }
            u32 dvx = 0, dvy = 0, dvw = 0, dvh = 0;
            GetDisplayViewport(dvx, dvy, dvw, dvh);
            resp["ok"] = true;
            resp["result"] = {
                {"scale", m_renderScale},
                // ★set 直後は「次フレームの Run ループ先頭」で反映されるので、
                //   ここで返る renderResolution はまだ 1 フレーム前の値であり得る。
                {"renderResolution",  {{"width", m_renderW}, {"height", m_renderH}}},
                {"displayResolution", {{"width", dvw},       {"height", dvh}}},
                {"pending", (m_renderW != 0) &&
                            (static_cast<u32>(std::lround(dvw * static_cast<double>(m_renderScale))) != m_renderW)},
                {"note", "シーン系 RT（sceneRT/深度/SSAO/TAA/SSR/SSGI/ブルーム/DoF/ゴッドレイ）の"
                         "解像度。UI と ImGui は常に表示解像度。dx12_screenshot はレンダー解像度、"
                         "dx12_screenshot_final は表示解像度で返る"}};
        });

    // ---- 深度プリパスの単独トグル（計画10 A2）----
    // 「そのシーンにオーバードローがどれだけあるか」を SSAO 生成のコストを混ぜずに測る道具。
    // gpuPassMs.depthPrepass（プリパス描画だけ）と gpuPassMs.mainScene の増減を突き合わせる。
    McpDefine("get_depth_prepass|set_depth_prepass", "enabled:bool", DX12E_MCP_HANDLER
        {
            if (method == "set_depth_prepass")
            {
                if (!params.contains("enabled"))
                    throw McpError(McpErr::InvalidParam, "need enabled (bool)");
                m_forceDepthPrepass = params.value("enabled", false);
                PersistSet("render_depth_prepass", m_forceDepthPrepass ? 1.0 : 0.0);
            }
            resp["ok"] = true;
            resp["result"] = {
                {"enabled", m_forceDepthPrepass},
                {"note", "深度プリパスを SSAO / コンタクトシャドウ / TAA / SSR / SSGI / DXR と"
                         "無関係に単独で走らせる。gpuPassMs.depthPrepass が描画だけの実測値、"
                         "prepassSsao はそれを含むプリパス一式。正射 / 2D ビューでは自動的に無効"}};
        });

    McpDefine("get_taa", "", DX12E_MCP_HANDLER
        {
            const auto& t = m_scene->GetTaaSettings();
            resp["ok"] = true;
            resp["result"] = {{"enabled", t.enabled}, {"sampleCount", t.sampleCount},
                              {"feedbackMin", t.feedbackMin}, {"feedbackMax", t.feedbackMax},
                              {"varianceGamma", t.varianceGamma}, {"jitterScale", t.jitterScale},
                              {"debugVelocity", t.debugVelocity},
                              // TAA が ON でも実際に走らない条件を明示する（誤診断を防ぐ）
                              {"active", t.enabled && m_taaPass && !m_camera->IsOrthographic()
                                         && !(m_editorCtx && m_editorCtx->view2D)},
                              {"fxaaSuppressed", t.enabled}};
        });

    McpDefine("set_taa", "debugVelocity:any,enabled:any,feedbackMax:any,feedbackMin:any,jitterScale:any,"
              "sampleCount:any,varianceGamma:any", DX12E_MCP_HANDLER
        {
            auto& t = m_scene->GetTaaSettings();
            t.enabled       = params.value("enabled",       t.enabled);
            t.sampleCount   = params.value("sampleCount",   t.sampleCount);
            t.feedbackMin   = params.value("feedbackMin",   t.feedbackMin);
            t.feedbackMax   = params.value("feedbackMax",   t.feedbackMax);
            t.varianceGamma = params.value("varianceGamma", t.varianceGamma);
            t.jitterScale   = params.value("jitterScale",   t.jitterScale);
            t.debugVelocity = params.value("debugVelocity", t.debugVelocity);
            resp["ok"] = true;
            resp["result"] = {{"applied", true}};
        });

    McpDefine("get_volumetric_fog", "", DX12E_MCP_HANDLER
        {
            const auto& f = m_scene->GetVolumetricFogSettings();
            resp["ok"] = true;
            resp["result"] = {
                {"enabled", f.enabled}, {"density", f.density},
                {"albedo", {f.albedo.x, f.albedo.y, f.albedo.z}},
                {"anisotropy", f.anisotropy},
                {"heightFalloff", f.heightFalloff}, {"heightRef", f.heightRef},
                {"distance", f.distance}, {"depthDistribution", f.depthDistribution},
                {"ambient", {f.ambient.x, f.ambient.y, f.ambient.z}},
                {"sunIntensity", f.sunIntensity}, {"lightScattering", f.lightScattering},
                {"temporal", f.temporal}, {"temporalBlend", f.temporalBlend},
                {"extendBeyondRange", f.extendBeyondRange}, {"debugMode", f.debugMode},
                // ON でも実際に走らない条件を明示する（誤診断を防ぐ）
                {"active", f.enabled && f.density > 0.0f && m_volumetricFogPass
                           && !m_camera->IsOrthographic()
                           && !(m_editorCtx && m_editorCtx->view2D)}};
        });

    McpDefine("set_volumetric_fog", "albedo:any,ambient:any,anisotropy:any,debugMode:any,density:any,depthDistribution:any,"
              "distance:any,enabled:any,extendBeyondRange:any,heightFalloff:any,heightRef:any,"
              "lightScattering:any,sunIntensity:any,temporal:any,temporalBlend:any", DX12E_MCP_HANDLER
        {
            auto& f = m_scene->GetVolumetricFogSettings();
            auto vec3 = [&](const char* key, DirectX::XMFLOAT3& dst)
            {
                if (params.contains(key) && params[key].is_array() && params[key].size() >= 3)
                {
                    dst.x = params[key][0].get<float>();
                    dst.y = params[key][1].get<float>();
                    dst.z = params[key][2].get<float>();
                }
            };
            f.enabled           = params.value("enabled",           f.enabled);
            f.density           = params.value("density",           f.density);
            vec3("albedo", f.albedo);
            f.anisotropy        = params.value("anisotropy",        f.anisotropy);
            f.heightFalloff     = params.value("heightFalloff",     f.heightFalloff);
            f.heightRef         = params.value("heightRef",         f.heightRef);
            f.distance          = params.value("distance",          f.distance);
            f.depthDistribution = params.value("depthDistribution", f.depthDistribution);
            vec3("ambient", f.ambient);
            f.sunIntensity      = params.value("sunIntensity",      f.sunIntensity);
            f.lightScattering   = params.value("lightScattering",   f.lightScattering);
            f.temporal          = params.value("temporal",          f.temporal);
            f.temporalBlend     = params.value("temporalBlend",     f.temporalBlend);
            f.extendBeyondRange = params.value("extendBeyondRange", f.extendBeyondRange);
            f.debugMode         = params.value("debugMode",         f.debugMode);
            resp["ok"] = true;
            resp["result"] = {{"applied", true}};
        });

    McpDefine("get_shadow_pcss|set_shadow_pcss", "blockerSearchTexels:number,enabled:any,lightTanAngle:number,maxPenumbraTexels:number,"
              "temporalDither:any", DX12E_MCP_HANDLER
        {
            // get/set を 1 本のハンドラで捌く（"a|b" 登録）。束ねる義務はもう無い（C1061 は根治済み）が、
            // 返り値が同一なのでこの形の方が読みやすい。
            auto& p = m_scene->GetShadowPcssSettings();
            if (method == "set_shadow_pcss")
            {
                p.enabled = params.value("enabled", p.enabled);
                if (params.contains("lightTanAngle"))
                    p.lightTanAngle = McpFloatParam(params, "lightTanAngle", p.lightTanAngle, 0.001f, 0.5f);
                if (params.contains("maxPenumbraTexels"))
                    p.maxPenumbraTexels = McpFloatParam(params, "maxPenumbraTexels", p.maxPenumbraTexels, 1.0f, 64.0f);
                if (params.contains("blockerSearchTexels"))
                    p.blockerSearchTexels = McpFloatParam(params, "blockerSearchTexels", p.blockerSearchTexels, 1.0f, 64.0f);
                p.temporalDither = params.value("temporalDither", p.temporalDither);
            }
            resp["ok"] = true;
            resp["result"] = {{"enabled", p.enabled},
                              {"lightTanAngle", p.lightTanAngle},
                              {"maxPenumbraTexels", p.maxPenumbraTexels},
                              {"blockerSearchTexels", p.blockerSearchTexels},
                              {"temporalDither", p.temporalDither},
                              // ON でも実際に走らない条件を明示する（誤診断を防ぐ）
                              {"active", p.enabled && m_scene->GetShadowsEnabled()
                                         && !m_camera->IsOrthographic()},
                              {"temporalDitherActive", p.enabled && p.temporalDither
                                         && m_scene->GetTaaSettings().enabled},
                              {"applied", method == "set_shadow_pcss"},
                              {"note", "OFF にすると従来の 3x3 PCF に戻る（絵はビット一致）。"
                                       "時間ディザは TAA 有効時のみ効く"}};
        });

    McpDefine("get_dxr|set_dxr", "aoCombineWithSsao:any,aoEnabled:any,aoIntensity:number,aoPower:number,aoRadius:number,"
              "aoRayCount:any,forceBuildTlas:any,maxInstances:any,shadowEnabled:any,"
              "shadowIntensity:number,shadowMaxDistance:number,shadowNormalBias:number,"
              "shadowSunAngle:number", DX12E_MCP_HANDLER
        {
            // get/set を 1 本のハンドラで捌く（"a|b" 登録）。
            //   RT サン影は既存のコンタクトシャドウ枠(t11)、RT-AO は既存の SSAO 枠(t8) へ書く。
            //   ルートシグネチャは 1 DWORD も増えない。
            auto& r = m_scene->GetRtSettings();
            if (method == "set_dxr")
            {
                if (!m_dxrEnabled)
                    throw McpError(McpErr::InvalidParam,
                        "this GPU does not support inline raytracing",
                        "DXR Tier 1.1 かつ Shader Model 6.5 が必要（RTX 20 系 / RX 6000 系以降）。"
                        "起動ログの \"DXR:\" 行に実際の Tier と SM が出る");
                r.shadowEnabled = params.value("shadowEnabled", r.shadowEnabled);
                if (params.contains("shadowSunAngle"))
                    r.shadowSunAngle = McpFloatParam(params, "shadowSunAngle", r.shadowSunAngle, 0.0f, 20.0f);
                if (params.contains("shadowNormalBias"))
                    r.shadowNormalBias = McpFloatParam(params, "shadowNormalBias", r.shadowNormalBias, 0.0f, 1.0f);
                if (params.contains("shadowMaxDistance"))
                    r.shadowMaxDistance = McpFloatParam(params, "shadowMaxDistance", r.shadowMaxDistance, 0.0f, 100000.0f);
                if (params.contains("shadowIntensity"))
                    r.shadowIntensity = McpFloatParam(params, "shadowIntensity", r.shadowIntensity, 0.0f, 1.0f);
                r.aoEnabled = params.value("aoEnabled", r.aoEnabled);
                if (params.contains("aoRadius"))
                    r.aoRadius = McpFloatParam(params, "aoRadius", r.aoRadius, 0.01f, 100.0f);
                if (params.contains("aoRayCount"))
                    r.aoRayCount = std::clamp(params.value("aoRayCount", r.aoRayCount), 1, 8);
                if (params.contains("aoIntensity"))
                    r.aoIntensity = McpFloatParam(params, "aoIntensity", r.aoIntensity, 0.0f, 1.0f);
                if (params.contains("aoPower"))
                    r.aoPower = McpFloatParam(params, "aoPower", r.aoPower, 0.01f, 8.0f);
                r.aoCombineWithSsao = params.value("aoCombineWithSsao", r.aoCombineWithSsao);
                if (params.contains("maxInstances"))
                    r.maxInstances = std::clamp(params.value("maxInstances", r.maxInstances), 0, 32768);
                r.forceBuildTlas = params.value("forceBuildTlas", r.forceBuildTlas);
            }
            const auto* gd = m_graphicsDevice.get();
            const int tier = gd ? static_cast<int>(gd->GetRaytracingTier()) : 0;
            const int sm   = gd ? static_cast<int>(gd->GetHighestShaderModel()) : 0;
            json stats = json::object();
            if (m_rtScene)
            {
                const auto& s = m_rtScene->GetStats();
                stats = {{"instances", s.instances}, {"blasCount", s.blasCount},
                         {"blasBytes", s.blasBytes}, {"blasTriangles", s.blasTriangles},
                         {"tlasBytes", s.tlasBytes}, {"scratchBytes", s.scratchBytes},
                         {"instanceDescBytes", s.instanceDescBytes},
                         {"skippedSkinned", s.skippedSkinned},
                         {"skippedTransparent", s.skippedTransparent},
                         {"droppedOverLimit", s.droppedOverLimit},
                         {"bytesPerTriangle", s.blasTriangles
                              ? static_cast<double>(s.blasBytes) / static_cast<double>(s.blasTriangles) : 0.0}};
            }
            resp["ok"] = true;
            resp["result"] = {{"supported", m_dxrEnabled},
                              {"raytracingTier", tier == 0 ? std::string("none")
                                   : std::to_string(tier / 10) + "." + std::to_string(tier % 10)},
                              {"highestShaderModel", std::to_string(sm >> 4) + "." + std::to_string(sm & 0xF)},
                              {"shadowEnabled", r.shadowEnabled},
                              {"shadowSunAngle", r.shadowSunAngle},
                              {"shadowNormalBias", r.shadowNormalBias},
                              {"shadowMaxDistance", r.shadowMaxDistance},
                              {"shadowIntensity", r.shadowIntensity},
                              {"aoEnabled", r.aoEnabled},
                              {"aoRadius", r.aoRadius},
                              {"aoRayCount", r.aoRayCount},
                              {"aoIntensity", r.aoIntensity},
                              {"aoPower", r.aoPower},
                              {"aoCombineWithSsao", r.aoCombineWithSsao},
                              {"maxInstances", r.maxInstances},
                              {"forceBuildTlas", r.forceBuildTlas},
                              // ON でも実際に走らない条件を明示する（誤診断を防ぐ）
                              {"shadowActive", m_rtShadowActiveThisFrame},
                              {"tlasReady", m_rtScene && m_rtScene->IsReady()},
                              {"stats", stats},
                              {"applied", method == "set_dxr"},
                              {"note", "RT 影は t11(コンタクトシャドウ枠)、RT-AO は t8(SSAO枠)へ書く。"
                                       "ルートシグネチャ増分ゼロ。スキンドと半透明は TLAS に入らないので "
                                       "CSM が担当する（min 合成）"}};
        });
}



} // namespace dx12e
