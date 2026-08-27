// ===========================================================================
// Application 内部ヘルパの実装（本文が大きく inline 化に向かないもの）
// ---------------------------------------------------------------------------
// Application.cpp から機械分割した実装 TU。分割の全体像は ApplicationInternal.h。
// ===========================================================================
#include "core/ApplicationInternal.h"

namespace dx12e
{

namespace appdetail
{

nlohmann::json PerfReportJson(const PerfSummary& s, bool vsync, float fpsLimit)
{
    auto r2 = [](double v) { return std::round(v * 100.0) / 100.0; };

    nlohmann::json gpu = nlohmann::json::object();
    for (u32 i = 0; i < GpuTimer::Count; ++i)
        gpu[GpuTimer::Name(i)] = r2(s.gpuMs[i]);

    nlohmann::json j{
        {"fps", r2(s.fps)},
        {"frameMs", {{"avg", r2(s.frameMs)}, {"min", r2(s.frameMsMin)},
                     {"max", r2(s.frameMsMax)}, {"p95", r2(s.frameMsP95)}}},
        {"cpu", {{"workMs", r2(s.workMs)}, {"fenceWaitMs", r2(s.fenceWaitMs)},
                 {"presentMs", r2(s.presentMs)}}},
        {"cpuScopeMs", [&] {
            nlohmann::json c = nlohmann::json::object();
            double named = 0;
            for (u32 i = 0; i < CpuScopeCount; ++i)
            {
                c[CpuScopeName(i)] = r2(s.cpuMs[i]);
                // ★picking / gizmo は editorUi の「内数」。合計に足すと二重計上になり、
                //   other が実際より小さく出る（＝計測できていない時間を過小報告する）。
                //   ここを間違えると「other は小さいから大丈夫」と誤読させてしまう。
                if (i == CpuPicking || i == CpuGizmo) continue;
                named += s.cpuMs[i];
            }
            // other = workMs のうちどのスコープにも入っていない分。
            // これが大きいなら「重いのは分かるがどこか分からない」状態＝スコープを足す合図。
            c["other"] = r2((std::max)(0.0, s.workMs - named));
            return c;
        }()},
        {"gpuPassMs", gpu},
        {"drawCalls", r2(s.draws)},
        {"culled", r2(s.culled)},
        {"triangles", static_cast<u64>(s.tris)},
        {"samples", s.samples},
        {"vsync", vsync},
        {"fpsLimit", fpsLimit},
    };

    // ---- 簡易ボトルネック解析 ----
    const double frame    = (std::max)(s.frameMs, 0.001);
    const double gpuTotal = s.gpuMs[GpuTimer::Total];
    const double waits    = s.fenceWaitMs + s.presentMs;              // GPU/表示待ちで CPU が寝てた時間
    const double cpuMs    = (std::max)(0.0, s.workMs - waits);        // CPU の実働

    std::string verdict = "balanced";
    if (fpsLimit > 0.0f && !vsync && s.fps >= fpsLimit * 0.95)
        verdict = "fps-limit-capped";
    else if (gpuTotal > frame * 0.7 || waits > frame * 0.5)
        verdict = (gpuTotal >= cpuMs) ? "gpu-bound" : "cpu-bound";
    else if (cpuMs > frame * 0.7)
        verdict = "cpu-bound";
    else if (vsync)
        verdict = "vsync-capped-or-balanced";

    std::vector<std::string> notes;
    if (verdict == "gpu-bound")
    {
        u32 worst = GpuTimer::Shadows;
        for (u32 i = GpuTimer::Shadows; i < GpuTimer::Count; ++i)
            if (s.gpuMs[i] > s.gpuMs[worst]) worst = i;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "GPU最大パス: %s (%.2fms)", GpuTimer::Name(worst), s.gpuMs[worst]);
        notes.push_back(buf);
        switch (worst)
        {
        case GpuTimer::Shadows:
            notes.push_back("影が重い: シャドウ解像度/カスケード数の削減、影を落とすライト数の見直しを検討"); break;
        case GpuTimer::MainScene:
            notes.push_back("メインパスが重い: triangles と drawCalls を確認。LOD/インスタンシング/ポリ削減を検討"); break;
        case GpuTimer::PrepassSSAO:
            notes.push_back("SSAO が重い: sampleCount 削減か SSAO OFF で切り分け"); break;
        case GpuTimer::PostFX:
            notes.push_back("ポストが重い: bloom/DoF/motionBlur/godRays を個別 OFF で切り分け"); break;
        case GpuTimer::VolumetricFog:
            notes.push_back("ボリュメトリックフォグが重い: distance を縮めるか lightScattering を OFF。"
                            "灯数が多いシーンでは散乱パスが 1 froxel あたり最大 128 灯を舐める"); break;
        default: break;
        }
    }
    if (verdict == "cpu-bound")
    {
        // どの CPU ブロックが食っているかまで名指しする（cpuScopeMs の最大値）。
        u32 cw = 0;
        for (u32 i = 1; i < CpuScopeCount; ++i)
            if (s.cpuMs[i] > s.cpuMs[cw]) cw = i;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "CPUバウンド: 最大ブロック %s (%.2fms / work %.2fms)",
                      CpuScopeName(cw), s.cpuMs[cw], s.workMs);
        notes.push_back(buf);
        switch (cw)
        {
        case CpuEditorUi:
            notes.push_back("エディタUIが重い: 階層パネルの項目数が多い。ゲーム実行時は掛からない"); break;
        case CpuBuildList:
            notes.push_back("描画リスト構築が重い: エンティティ数を減らすか、静的物の事前計算を検討"); break;
        case CpuShadowRec:
            notes.push_back("影のコマンド記録が重い: カスケード数削減 or 影用インスタンシング/バッチングを検討"); break;
        case CpuMainRec:
            notes.push_back("メインのコマンド記録が重い: drawCalls を減らす(インスタンシング/マージ)"); break;
        case CpuPicking:
            notes.push_back("シーンビューのピッキングが重い: 高ポリメッシュが密集している(editorUi の内数)"); break;
        case CpuGizmo:
            notes.push_back("ギズモが重い: マルチ選択の数が多い(editorUi の内数)"); break;
        default:
            notes.push_back("Update が重い: Lua/物理/アニメ更新を確認"); break;
        }
    }
    if (s.draws > 3000)
        notes.push_back("drawCalls が多い(平均" + std::to_string(static_cast<int>(s.draws)) + "/フレーム)");
    if (s.tris > 5e6)
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "三角形数が多い(平均 %.1fM/フレーム)", s.tris / 1e6);
        notes.push_back(buf);
    }
    if (vsync)
        notes.push_back("VSync ON: 上限測定には video 設定で VSync OFF 推奨");

    j["analysis"] = {{"verdict", verdict}, {"notes", notes}};
    return j;
}

nlohmann::json McpComponentSchema()
{
    using nlohmann::json;
    auto F = [](const char* name, const char* type, json def) {
        return json{{"name", name}, {"type", type}, {"default", std::move(def)}};
    };
    auto C = [](const char* key, bool settable, bool removable, json fields, const char* note = "") {
        json c{{"jsonKey", key}, {"settable", settable}, {"removable", removable},
               {"luaAccessible", LuaReadableComponent(key)}, {"fields", std::move(fields)}};
        if (note[0]) c["note"] = note;
        return c;
    };
    json comps = json::array();
    comps.push_back(C("transform", true, false, json::array({
        F("position", "float3", json::array({0, 0, 0})),
        F("rotation", "float3 (euler degrees)", json::array({0, 0, 0})),
        F("scale", "float3", json::array({1, 1, 1})),
        F("quaternion", "float4 (x,y,z,w). Setting it also syncs rotation(euler), because the "
                        "scene file only stores euler — otherwise the pose reverts on reload.",
          json::array({0, 0, 0, 1})),
    }), "core; cannot be removed. Prefer dx12_set_transform for position/rotation/scale."));
    comps.push_back(C("meshRenderer", false, false, json::array({
        F("modelPath", "string (assets-relative)", ""),
    }), "read-only via MCP; create with dx12_spawn_model/dx12_create_entity. Use dx12_set_pbr for material, "
        "dx12_set_mesh_shader for shaderPath (custom HLSL from dx12_create_shader). UV scroll "
        "(uvScrollU/V) and sprite-sheet flipbook (animFrames/animFps/animCols/animRow/animRows/animMode) "
        "are set in the Inspector 'UV & Anim' section or from Lua: scene:setMeshUvScroll / scene:setMeshAnim."));
    comps.push_back(C("pointLight", true, true, json::array({
        F("color", "float3", json::array({1, 1, 1})), F("intensity", "float", 1.0), F("range", "float", 10.0),
        F("castShadows", "bool (max 2 simultaneous, nearest-to-camera wins)", false),
    })));
    comps.push_back(C("directionalLight", true, true, json::array({
        F("direction", "float3", json::array({0, -1, 0})), F("color", "float3", json::array({1, 1, 1})),
        F("intensity", "float", 1.0), F("ambient", "float", 0.25),
    })));
    comps.push_back(C("spotLight", true, true, json::array({
        F("color", "float3", json::array({1, 1, 1})), F("intensity", "float", 3.0), F("range", "float", 15.0),
        F("direction", "float3", json::array({0, -1, 0})),
        F("innerConeDeg", "float", 18.0), F("outerConeDeg", "float", 28.0),
        F("castShadows", "bool (max 4 simultaneous, nearest-to-camera wins)", false),
    })));
    comps.push_back(C("camera", true, true, json::array({
        F("fovDegrees", "float", 60.0), F("nearClip", "float", 0.1), F("farClip", "float", 1000.0),
        F("isActive", "bool", false), F("projection", "int (0=Perspective,1=Orthographic)", 0),
        F("orthoSize", "float", 10.0),
    })));
    comps.push_back(C("rigidBody", true, true, json::array({
        F("motionType", "int (0=Static,1=Kinematic,2=Dynamic)", 2), F("mass", "float", 1.0),
        F("restitution", "float", 0.4), F("friction", "float", 0.3),
        F("linearDamping", "float", 0.02), F("angularDamping", "float", 0.01), F("useGravity", "bool", true),
        F("continuousCollision", "bool (CCD。弾丸/投擲物など1フレームで自分の厚みより長く動く物だけ true。既定 false は薄い壁をすり抜ける)", false),
    })));
    comps.push_back(C("boxCollider", true, true, json::array({
        F("halfExtents", "float3", json::array({0.5, 0.5, 0.5})), F("offset", "float3", json::array({0, 0, 0})),
    })));
    comps.push_back(C("sphereCollider", true, true, json::array({
        F("radius", "float", 0.5), F("offset", "float3", json::array({0, 0, 0})),
    })));
    comps.push_back(C("capsuleCollider", true, true, json::array({
        F("radius", "float", 0.5), F("halfHeight", "float", 1.0), F("offset", "float3", json::array({0, 0, 0})),
    })));
    comps.push_back(C("characterController", true, true, json::array({
        F("radius", "float", 0.4), F("halfHeight", "float", 0.6), F("offset", "float3", json::array({0, 0, 0})),
        F("mass", "float", 70.0), F("maxSlopeDeg", "float", 50.0), F("stepHeight", "float", 0.3),
        F("jumpSpeed", "float", 6.0), F("gravityScale", "float", 1.0),
    }), "mutually exclusive with rigidBody; do not add both."));
    comps.push_back(C("convexHullCollider", false, true, json::array({}),
        "auto-generated from mesh on load; not settable via MCP. Removable."));
    comps.push_back(C("sprite2d", true, true, json::array({
        F("texturePath", "string (assets-relative)", ""), F("layer", "int", 0),
        F("size", "float2", json::array({1, 1})), F("uvMin", "float2", json::array({0, 0})),
        F("uvMax", "float2", json::array({1, 1})), F("color", "float4 (rgba)", json::array({1, 1, 1, 1})),
        F("worldSpace", "bool", true), F("billboard", "bool", false),
        F("shaderPath", "string (assets-relative, worldSpace only)", ""), F("shaderAlphaBlend", "bool", false),
        F("effectValue", "float (generic progress/strength for custom shader)", 0.0),
        F("shaderParams", "float4 (generic params for custom shader, TEXCOORD2)", json::array({0, 0, 0, 0})),
        F("animFrames", "int (flipbook total frames; 0=off. When >0, uvMin/uvMax are auto-set per frame)", 0),
        F("animFps", "float (flipbook playback speed, frames/sec)", 8.0),
        F("animCols", "int (sprite-sheet columns; 0=animFrames i.e. single-row strip)", 0),
        F("animRow", "int (start row in sheet, for multi-animation sheets)", 0),
        F("animRows", "int (total rows in sheet; 0=auto: animRow+ceil(animFrames/animCols))", 0),
        F("animMode", "int (0=loop, 1=once (hold last frame), 2=ping-pong)", 0),
        F("scrollU", "float (UV scroll speed, units/sec; ignored while animFrames>0)", 0.0),
        F("scrollV", "float (UV scroll speed, units/sec; ignored while animFrames>0)", 0.0),
    }), "Use dx12_set_sprite_shader for shaderPath (custom HLSL, world-space only; different vertex/root-"
        "signature contract than meshRenderer shaders, see docs/AUTHORING.md)."));
    comps.push_back(C("tags", true, true, json::array({}),
        "data is a STRING ARRAY, e.g. set_component(component='tags', data=[\"enemy\",\"boss\"])."));
    comps.push_back(C("data", true, true, json::array({}),
        "key->{t,v} map. t in number|bool|string|vec3 (int は number 扱い・get_entity は number で返す). e.g. data={\"hp\":{\"t\":\"number\",\"v\":100}}."));
    comps.push_back(C("audioSource", true, true, json::array({
        F("clipPath", "string (assets-relative)", ""), F("volume", "float", 1.0), F("loop", "bool", false),
        F("spatial", "bool", true), F("playOnStart", "bool", true),
        F("minDistance", "float", 1.0), F("maxDistance", "float", 30.0),
    })));
    comps.push_back(C("particleEmitter", true, true, json::array({
        F("kind", "int (0=Glow,1=Fire,2=Smoke,3=Spark,4=Magic,5=Electric,6=Ring,7=Star)", 0),
        F("blend", "int (0=Additive,1=Alpha)", 0), F("rate", "float (per sec)", 30.0),
        F("orient", "int (0=Billboard,1=Horizontal XZ,2=Vertical XY)", 0),
        F("playOnStart", "bool", true), F("looping", "bool", true), F("duration", "float", 1.0),
        F("dir", "float3", json::array({0, 1, 0})), F("spread", "float", 0.4), F("speed", "float", 3.0),
        F("speedVar", "float", 0.4), F("size", "float", 0.3), F("sizeEnd", "float", 0.0),
        F("life", "float", 0.8), F("lifeVar", "float", 0.3),
        F("color", "float3", json::array({1, 0.6, 0.2})), F("colorEnd", "float3", json::array({1, 0.12, 0.05})),
        F("intensity", "float", 3.0), F("gravity", "float", 0.0), F("drag", "float", 1.0),
        F("up", "float", 0.0), F("stretch", "float", 0.0),
        F("colorMid", "float3 (set implies hasColorMid=true)", json::array({1, 0.6, 0.2})),
        F("hasColorMid", "bool (3-key color curve start→mid→end)", false),
        F("turbStrength", "float (>0 = curl-noise turbulence for smoke/fire)", 0.0),
        F("turbFreq", "float", 1.0),
        F("sizeMid", "float (>=0 = 3-key size curve)", -1.0),
        F("distort", "float (>0 = heat-haze/shockwave distortion particles)", 0.0),
        F("light", "bool (brightest N particles become real point lights)", false),
        F("lightRange", "float", 3.0),
        F("flicker", "float (0..1 emissive flicker)", 0.0), F("flickerFreq", "float", 18.0),
        F("gpu", "bool (GPU compute particles, max 131072, additive only; distort/light/sizeMid/alpha-blend unsupported)", false),
        F("texturePath", "string (assets-relative; empty = procedural look)", ""),
    })));
    comps.push_back(C("trailRenderer", true, true, json::array({
        F("emitting", "bool", true), F("width", "float (world units)", 0.25),
        F("life", "float (sec = ribbon length)", 0.5),
        F("color", "float3", json::array({0.4, 0.8, 1.0})), F("colorEnd", "float3", json::array({0.1, 0.2, 1.0})),
        F("intensity", "float (HDR, >1 blooms)", 2.0), F("blend", "int (0=Additive,1=Alpha)", 0),
        F("minDist", "float (min movement to drop a point)", 0.03),
    }), "camera-facing ribbon trail (sword slash / projectile / magic tail). Follows the entity's world position."));
    comps.push_back(C("decal", true, true, json::array({
        F("atlasUV", "float4 (u0,v0,du,dv) rect inside the scene decal atlas", json::array({0, 0, 1, 1})),
        F("atlasUVNormal", "float4 rect for the normal map (dv<=0 = no normal map)", json::array({0, 0, 0, 0})),
        F("tint", "float3", json::array({1, 1, 1})), F("opacity", "float 0..1", 1.0),
        F("emissive", "float3 (HDR, added)", json::array({0, 0, 0})),
        F("normalStrength", "float", 1.0),
        F("roughness", "float (<0 = keep receiver's)", -1.0),
        F("metallic", "float (<0 = keep receiver's)", -1.0),
        F("angleFadeDeg", "float (fade out on slopes steeper than this)", 60.0),
        F("fadeEdge", "float (edge fade width in local units)", 0.1),
        F("sortOrder", "int (lower = underneath)", 0),
    }), "projected decal (bullet hole / blood / dirt / puddle). The Transform's scale is the projection box; "
        "it projects along local -Y so an unrotated decal lands on the floor. The texture comes from the "
        "scene-level 'decalAtlas' + this atlasUV rect. Max 256 per scene / 16 per cluster."));
    comps.push_back(C("networkIdentity", true, true, json::array({
        F("interestRadius", "float (0 = always relevant, no distance culling)", 0.0),
        F("serverAuthority", "bool (reserved; nothing reads it yet)", true),
    }), "marks the entity for multiplayer replication (host assigns netId). Pair with networkTransform. "
        "Use dx12_net_setup + dx12_play to test."));
    comps.push_back(C("networkTransform", true, true, json::array({
        F("syncMode", "int (0=interpolated proxy, 1=owner-predicted)", 0),
        F("syncPosition", "bool", true), F("syncRotation", "bool", true), F("syncScale", "bool", false),
        F("interpDelayMs", "float (jitter buffer)", 100.0), F("snapDistance", "float (teleport threshold)", 5.0),
    }), "replicates Transform snapshots. Requires networkIdentity on the same entity."));
    comps.push_back(C("skeletalAnimation", false, false, json::array({}),
        "read-only via MCP (created by model load). Control playback with dx12_play_anim / dx12_get_anim_state."));
    comps.push_back(C("animatorController", true, true, json::array({
        F("graphPath", "string (assets-relative .animfsm)", ""),
        F("playOnStart", "bool", true),
        F("speed", "float (graph-wide playback rate)", 1.0),
        F("applyRootMotion", "bool (not implemented yet; keep false)", false),
        F("eventChannel", "string (prefix prepended to clip event names)", ""),
    }), "Animation state machine driving the Animator. Requires skeletalAnimation on the same entity. "
        "The graph structure (states/transitions/blend trees/layers/masks/clipEvents) lives in the .animfsm "
        "JSON asset - edit it with Write/Edit, then dx12_open_scene to reload. Inspect with "
        "dx12_describe_anim_graph, drive with dx12_set_anim_param, force a state with dx12_play_anim {state}."));
    comps.push_back(C("footIK", true, true, json::array({
        F("enabled", "bool", true),
        F("weight", "float (0..1 overall strength)", 1.0),
        F("leftHipBone", "string (empty = auto-detect from common naming)", ""),
        F("leftKneeBone", "string", ""), F("leftFootBone", "string", ""),
        // ★toe は解決までされるが ApplyFootIK が一度も読まない（つま先ピボット用の
        //   受け口として置いてあるだけ）。設定しても何も起きないと明記する。
        F("leftToeBone", "string (RESERVED - resolved but never used by the IK solver)", ""),
        F("rightHipBone", "string", ""), F("rightKneeBone", "string", ""),
        F("rightFootBone", "string", ""),
        F("rightToeBone", "string (RESERVED - resolved but never used by the IK solver)", ""),
        F("pelvisBone", "string (empty = root bone)", ""),
        F("rayUpOffset", "float (metres above the ankle to start the ray)", 0.5),
        F("rayLength", "float (total ray length in metres)", 1.0),
        F("footHeight", "float (ankle height above ground in the rest pose)", 0.1),
        F("maxPelvisDrop", "float (metres)", 0.5),
        F("maxFootPitchDeg", "float (degrees)", 45.0),
        F("smoothTime", "float (seconds, exponential smoothing time constant)", 0.1),
        F("fadeOutTime", "float (seconds)", 0.15),
        F("alignToNormal", "bool", true),
        F("kneeForward", "float3 (model space direction the knee points)", json::array({0, 0, 1})),
    }), "Foot placement IK. Requires skeletalAnimation on the same entity. Raycasts the ground, "
        "matches ankle height and orientation, and drops the pelvis to reach the lower foot. "
        "ONLY RUNS IN PLAY MODE (physics bodies exist only while playing). Bone names left empty "
        "are auto-detected from common rig naming; check the resolved result in dx12_get_anim_state's "
        "footIK block. Uses PhysicsSystem::Raycast for true surface normals."));
    comps.push_back(C("trigger", true, true, json::array({
        F("shape", "int (0=Box,1=Sphere)", 0), F("halfExtents", "float3", json::array({1, 1, 1})),
        F("radius", "float", 1.0), F("offset", "float3", json::array({0, 0, 0})),
        F("filter", "string (entity name; empty=Player). Derived from filterGuid when set — "
                    "the engine rewrites it from the target's current name on save.", ""),
        F("filterGuid", "string (16-hex EntityGuid; authoritative over filter). Omit it and the "
                        "engine promotes filter->guid on load. Set it to \"\" to force name lookup.", ""),
        F("once", "bool", false),
        F("actions", "array of {when:int(0=Enter,1=Exit,2=Stay), type:int(0..10), target:string, "
                     "targetGuid:string(16-hex, authoritative over target), str:string, num:number, vec:float3}", json::array()),
    })));
    // ★これは「動く部品」ではなく**ただのパラメータ置き場**。C++ に更新システムは無い
    //   (Gimmick を読むのは保存/復元・Inspector・scene:gimmicks() だけ。grep 済み)。
    //   説明が無いと「kind=SpikePulse, period=4 を置いたのに動かない」で必ず詰まる。
    comps.push_back(C("gimmick", true, true, json::array({
        F("kind", "int (0=StaticWall,1=SpikePulse,2=SlideX,3=SlideZ)", 0), F("period", "float", 4.0),
        F("phase", "float (0..1)", 0.0), F("amplitude", "float", 1.6), F("threshold", "float (0..1)", 0.5),
        F("solid", "bool", true), F("deadly", "bool", false),
    }), "DATA ONLY - nothing in C++ moves it. Adding this component makes the entity do nothing by itself. "
        "A Lua script must read the parameters and drive the motion, e.g. "
        "`for _,g in ipairs(scene:gimmicks()) do local t=(time.now()/g.period+g.phase)%1 ... end`. "
        "scene:gimmicks() returns {e=Entity, name, kind, period, phase, amplitude, threshold, solid, deadly}. "
        "kind/threshold/solid/deadly are conventions your script must honour, not engine behaviour."));
    comps.push_back(C("luaScript", false, true, json::array({
        F("scriptPath", "string (assets-relative)", ""), F("enabled", "bool", true),
    }), "attach via dx12_attach_lua_component (not set_component). Removable via MCP."));

    // --- 地形 / スカルプト（高さ配列・頂点配列そのものは JSON に載らない。パスだけ持つ）---
    comps.push_back(C("terrain", false, false, json::array({
        F("resolution", "int (1 辺のサンプル数。16..512、内部で 4 の倍数へ丸め)", 128),
        F("worldSize", "float (1 辺のワールド長 m。セル幅 = worldSize/(resolution-1))", 200.0),
        F("maxHeight", "float (ブラシの高さクランプ ±この値)", 200.0),
        F("heightmapPath", "string (assets-relative .hf。空=未保存)", ""),
        F("uvScale", "float (地形全体での UV 繰り返し数)", 24.0),
        F("color", "float4 rgba (頂点色。マテリアル未割当時の見た目)", json::array({0.42, 0.50, 0.32, 1.0})),
    }), "read-only via set_component (replacing the component would detach the live height array "
        "from the mesh and the collider). The height array itself is NOT in the scene JSON — it lives "
        "in assets/terrain/<name>.hf. Use the dedicated tools: dx12_terrain_create (also updates "
        "worldSize/maxHeight/uvScale/color on an existing terrain), dx12_terrain_generate (fBm presets), "
        "dx12_terrain_sculpt (brush strokes), dx12_terrain_erode, dx12_terrain_sample (height/normal "
        "queries). Collision is a Jolt HeightFieldShape reading the same array, so sculpting moves the "
        "collision too (a Static RigidBody is added automatically)."));
    comps.push_back(C("sculptMesh", false, false, json::array({
        F("meshPath", "string (assets-relative .smsh。空=未保存)", ""),
        F("uvScale", "float", 1.0),
        F("collision", "bool (彫った形の MeshShape コライダーを作る)", true),
        F("color", "float4 rgba (頂点色)", json::array({0.72, 0.70, 0.66, 1.0})),
    }), "free-form vertex sculpt (caves / arches / rocks — things a height field cannot do). "
        "read-only via set_component for the same reason as terrain. Vertex array is NOT in the scene "
        "JSON (assets/sculpt/<name>.smsh). Use dx12_sculpt_create (also updates uvScale/color/collision "
        "on an existing one) or dx12_sculpt_make_editable on an existing model, then dx12_sculpt_brush. "
        "Topology never changes (only vertex positions move), so the collider follows the sculpted shape."));

    // --- ゲーム内UI(retained-mode)。create は dx12_create_entity type=ui_*(部品構成済みで生成)、
    // 調整はここの jsonKey で set_component。ツリーは dx12_set_parent + uiRect.order(兄弟描画順)。
    comps.push_back(C("uiCanvas", true, true, json::array({
        F("refWidth", "float (reference resolution)", 1920.0), F("refHeight", "float", 1080.0),
        F("scaleMode", "int (0=ScaleToFit letterbox, 1=ConstantPixel, 2=StretchToFill no margins)", 0),
        F("sortOrder", "int (canvas draw order)", 0), F("visible", "bool", true),
    }), "UI tree root. Children (via dx12_set_parent) with uiRect become UI elements."));
    comps.push_back(C("uiRect", true, true, json::array({
        F("anchorMin", "float2 (0..1 in parent)", json::array({0.5, 0.5})),
        F("anchorMax", "float2", json::array({0.5, 0.5})),
        F("pivot", "float2", json::array({0.5, 0.5})),
        F("offsetMin", "float2 (px from anchor)", json::array({-50, -50})),
        F("offsetMax", "float2", json::array({50, 50})),
        F("visible", "bool", true),
        F("order", "int (sibling draw order; larger = front)", 0),
        F("rotation", "float (visual rotation deg, CW, around pivot; children rotate too)", 0.0),
        F("skewX", "float (horizontal skew deg; parallelogram banners)", 0.0),
        F("clipChildren", "bool (mask: children clipped to this rect; wipes/marquees. "
          "Axis-aligned scissor = rotation/skewX disabled on this node)", false),
    }), "Layout: rectMin = parentMin + parentSize*anchorMin + offsetMin (same for max). "
        "Full-stretch = anchorMin[0,0] anchorMax[1,1] offsets 0. rotation/skewX are "
        "visual-only (layout stays axis-aligned); ignored on uiScrollView/clipChildren nodes."));
    comps.push_back(C("uiImage", true, true, json::array({
        F("texturePath", "string (assets-relative; empty = solid color rect)", ""),
        F("color", "float4 (rgba)", json::array({1, 1, 1, 1})),
        F("uvMin", "float2", json::array({0, 0})), F("uvMax", "float2", json::array({1, 1})),
        F("sliceBorder", "float4 (9-slice px L,T,R,B; all 0 = off)", json::array({0, 0, 0, 0})),
        F("cornerRadius", "float (solid color rect only)", 0.0),
        F("raycastBlock", "bool (blocks clicks like Unity raycastTarget)", true),
        F("fillAmount", "float 0..1 (HP bar/gauge)", 1.0),
        F("fillDir", "int (0=fromLeft,1=fromRight,2=fromBottom,3=fromTop,"
          "4=radialCW,5=radialCCW; radial = cooldown sweep, works on rect too)", 0),
        F("fillOrigin", "float (radial fill start angle deg; 0 = top, CW positive)", 0.0),
        F("shape", "int (0=rect,1=ellipse,2=ring,3=diamond,4=hexagon,5=triangleUp; non-rect "
          "ignores cornerRadius/9-slice; textures are masked to the shape; ring is solid-color "
          "arc gauge whose fill is always radial)", 0),
        F("ringThickness", "float (ring band thickness px; shape=2)", 8.0),
        F("uvScroll", "float2 (uv/sec pattern scroll; tile with uvMax>1; not for 9-slice/shapes)",
          json::array({0, 0})),
        F("animFrames", "int (sprite-sheet flipbook total frames; 0=off. When >0, uvMin/uvMax and "
          "uvScroll are ignored; not for 9-slice/shapes)", 0),
        F("animFps", "float (flipbook playback speed, frames/sec)", 8.0),
        F("animCols", "int (sprite-sheet columns; 0=animFrames i.e. single-row strip)", 0),
        F("animRow", "int (start row in sheet, for multi-animation sheets)", 0),
        F("animRows", "int (total rows in sheet; 0=auto: animRow+ceil(animFrames/animCols))", 0),
        F("animMode", "int (0=loop, 1=once (hold last frame), 2=ping-pong)", 0),
        F("gradientDir", "int (0=off,1=horizontal,2=vertical,3=diagonal,4=radial center→edge)", 0),
        F("gradientColor2", "float4 (gradient end color; alpha ignored)", json::array({1, 1, 1, 1})),
        F("gradientScrollSpeed", "float (gloss sweep: !=0 replaces static gradient with a "
          "gradientColor2 light band sweeping along gradientDir; cycles/sec, negative = reverse)",
          0.0),
        F("outlineWidth", "float (border px; 0 = off; follows cornerRadius)", 0.0),
        F("outlineColor", "float4", json::array({0, 0, 0, 1})),
        F("outlineStyle", "int (0=solid,1=dashed,2=corner brackets(sci-fi HUD); rect only; "
          "1/2 ignore cornerRadius)", 0),
        F("outlineDash", "float (dash length px / bracket arm length px)", 12.0),
        F("segments", "int (segmented gauge: draws n-1 separator lines across the bar; "
          "0 = off; rect + linear fill only)", 0),
        F("segmentGap", "float (separator thickness px)", 3.0),
        F("segmentColor", "float4", json::array({0, 0, 0, 0.7})),
        F("shadowColor", "float4 (drop shadow; alpha 0 = off; shape approximation)",
          json::array({0, 0, 0, 0})),
        F("shadowOffset", "float2 (px)", json::array({2, 2})),
        F("shadowSoftness", "float (blur spread px; 0 = sharp)", 4.0),
    })));
    comps.push_back(C("uiText", true, true, json::array({
        F("text", "string", "テキスト"), F("fontSize", "float", 24.0),
        F("color", "float4 (rgba)", json::array({1, 1, 1, 1})),
        F("alignH", "int (0=left,1=center,2=right)", 1),
        F("alignV", "int (0=top,1=center,2=bottom)", 1), F("wrap", "bool", false),
        F("outlineWidth", "float (8-dir text outline px; 0 = off)", 0.0),
        F("outlineColor", "float4", json::array({0, 0, 0, 1})),
        F("shadowColor", "float4 (alpha 0 = off)", json::array({0, 0, 0, 0})),
        F("shadowOffset", "float2 (px)", json::array({1, 1})),
        F("fontPath", "string (assets-relative .ttf/.otf; empty = default Yu Gothic)", ""),
        F("typewriterSpeed", "float (chars/sec typewriter reveal in Play mode; 0 = off; "
          "UTF-8 codepoint-safe; Lua setUiText restarts it)", 0.0),
        F("letterSpacing", "float (px between glyphs; negative = tighter; enables per-glyph "
          "mode; not with wrap)", 0.0),
        F("charAnim", "int (per-glyph anim: 0=off,1=wave,2=jitter,3=rainbow; not with wrap)", 0),
        F("charAnimAmount", "float (wave/jitter amplitude px)", 4.0),
        F("charAnimSpeed", "float (Hz)", 2.0),
        F("gradientDir", "int (text body gradient: 0=off,1=horizontal,2=vertical; "
          "gold titles etc.)", 0),
        F("gradientColor2", "float4 (gradient end color; alpha ignored)",
          json::array({1, 1, 1, 1})),
        F("rich", "bool (inline tags in text: [c=RRGGBB]..[/c] color span, [wave]..[/wave], "
          "[shake]..[/shake], [rainbow]..[/rainbow] per-span char anim reusing "
          "charAnimAmount/Speed; flat, no nesting; unclosed tag runs to end; unknown tags "
          "drawn literally; forces per-glyph; disabled with wrap; gradientDir ignored)", false),
    })));
    comps.push_back(C("uiButton", true, true, json::array({
        F("onClickEvent", "string (emitted to Lua events on click; empty = none)", ""),
        F("normalColor", "float4", json::array({1, 1, 1, 1})),
        F("hoverColor", "float4", json::array({0.85, 0.85, 0.85, 1})),
        F("pressedColor", "float4", json::array({0.65, 0.65, 0.65, 1})),
        F("interactable", "bool", true),
        F("hoverSound", "string (assets-relative wav; empty = silent)", ""),
        F("clickSound", "string", ""),
    }), "Needs uiImage on same entity for state tint. Lua: events:on(onClickEvent, fn); "
        "e.source = entity id."));
    comps.push_back(C("uiSlider", true, true, json::array({
        F("value", "float (real value minValue..maxValue)", 0.5),
        F("minValue", "float", 0.0), F("maxValue", "float", 1.0),
        F("step", "float (0 = continuous)", 0.0),
        F("onChangeEvent", "string (e.value = real value)", ""),
        F("trackColor", "float4", json::array({0.22, 0.22, 0.27, 1})),
        F("fillColor", "float4", json::array({0.3, 0.55, 1, 1})),
        F("knobColor", "float4", json::array({1, 1, 1, 1})),
        F("interactable", "bool", true),
    }), "Self-drawn (no uiImage needed). uiRect is the operable area."));
    comps.push_back(C("uiToggle", true, true, json::array({
        F("isOn", "bool", false),
        F("onChangeEvent", "string (e.value = 1/0)", ""),
        F("boxColor", "float4", json::array({0.22, 0.22, 0.27, 1})),
        F("checkColor", "float4", json::array({0.3, 0.55, 1, 1})),
        F("interactable", "bool", true),
    }), "uiRect = the checkbox itself. Label = child uiText entity (ui_toggle spawns one)."));
    comps.push_back(C("uiScrollView", true, true, json::array({
        F("vertical", "bool", true), F("horizontal", "bool", false),
        F("scrollX", "float (px; auto-clamped to content)", 0.0), F("scrollY", "float", 0.0),
        F("wheelSpeed", "float (px per wheel notch)", 48.0),
        F("showBar", "bool", true), F("barColor", "float4", json::array({1, 1, 1, 0.35})),
        F("dragScroll", "bool (drag/flick inertia scrolling; a >6px drag cancels button "
          "presses inside so scrolling never misfires clicks)", true),
        F("flickDecay", "float (flick inertia exponential decay per second; 0 = no inertia, "
          "stops on release)", 4.0),
    }), "uiRect = viewport. Children are clipped + scrolled (clicks clipped too). "
        "Hang children via dx12_set_parent."));
    comps.push_back(C("uiLayout", true, true, json::array({
        F("mode", "int (0=VBox vertical stack,1=HBox horizontal,2=Grid row-major)", 0),
        F("cellW", "float (cell width px; VBox: 0 = full inner width)", 200.0),
        F("cellH", "float (cell height px; HBox: 0 = full inner height)", 60.0),
        F("spacing", "float (px between cells)", 8.0),
        F("padding", "float4 (inner padding L,T,R,B px)", json::array({0, 0, 0, 0})),
        F("gridCols", "int (grid columns; mode=2)", 4),
    }), "Auto-layout container: direct children (with uiRect) each get a sequential cell rect "
        "as their parent rect — no manual offset math for lists/toolbars/inventories. "
        "Children anchor within their cell (full-stretch anchors = fill the cell). "
        "Combine with uiScrollView by putting uiLayout on a child content node."));
    comps.push_back(C("uiAnimator", true, true, json::array({
        F("showAnim", "int (0=none,1=fade,2=pop,3=fromLeft,4=fromRight,5=fromTop,6=fromBottom,"
          "7=spinIn,8=bounceDrop,9=flipIn,10=shakeIn,11=flipInX(door/card))", 1),
        F("showDuration", "float (sec)", 0.35), F("showDelay", "float (sec)", 0.0),
        F("showEasing", "int (0=linear,1=in,2=out,3=inOut,4=back,5=bounce,6=elastic,"
          "7=expo,8=inBack,9=inOutBack,10=quint,11=sine)", 2),
        F("slideOffset", "float (px)", 80.0),
        F("hoverScale", "float (needs uiButton)", 1.05), F("pressScale", "float", 0.95),
        F("hoverSpeed", "float", 14.0),
        F("loopAnim", "int (0=none,1=float,2=pulse,3=blink,4=spin,5=sway)", 0),
        F("loopSpeed", "float (Hz; spin = revolutions/sec)", 2.0),
        F("loopAmount", "float (float=px, pulse/blink=ratio, sway=deg)", 6.0),
    }), "Play-mode only. Show anim replays on Lua scene:showUi()."));
    return json{{"components", std::move(comps)}};
}

nlohmann::json McpLuaApi()
{
    using nlohmann::json;
    auto O = [](const char* name, const char* obtainedBy, json members) {
        json o{{"name", name}, {"members", std::move(members)}};
        if (obtainedBy[0]) o["obtainedBy"] = obtainedBy;
        return o;
    };
    json objects = json::array();
    objects.push_back(O("callbacks", "(各 Lua コンポーネントが任意で定義)", json::array({
        "OnStart(self)       — Play開始/アタッチ時に1回。第1引数は self(table)",
        "OnUpdate(self, dt)  — 毎フレーム。第1引数 self、第2引数 dt(秒)。コンポーネントは self が必須",
        "注: グローバル(シーン)スクリプトは OnUpdate(dt)(self 無し)。コンポーネントは OnUpdate(self, dt)",
    })));
    objects.push_back(O("entity", "scene:findEntity(name) / scene:spawn* / physics:overlap*", json::array({
        "isValid() -> bool",
        "name  (string, read-only property)",
        "transform  (Transform getter。フィールドは書込可: entity.transform.position = Vec3.new(x,y,z)。ただし entity.transform 自体の再代入は read-only) — 唯一直接読めるコンポーネントデータ",
        // ★型名は ScriptEngine.cpp:507-544 の if 連鎖と 1:1。ここが短いと「対応していない」と
        //   誤解されて使われなくなるので、増やしたら必ず両方直すこと。
        "hasComponent(type:string) -> bool  (type: Transform,NameTag,Tag,DataComponent,MeshRenderer,"
        "SkeletalAnimation,NodeAnimation,GridPlane,PointLight,DirectionalLight,SpotLight,Camera,Sprite2D,"
        "AudioSource,Gimmick,RigidBody,BoxCollider,SphereCollider,CapsuleCollider,ConvexHullCollider,"
        "CharacterController,LuaScript,ParticleEmitter,TrailRenderer,DecalComponent,Trigger,UICanvas,UIRect,"
        "UIImage,UIText,UIButton,UISlider,UIToggle,UIScrollView,UILayout,UIAnimator,AnimatorController,FootIK)"
        "  ※知らない型名は false ではなくログに警告が出る(タイプミスを黙って握り潰さない)",
        "playAnim(clipIndex:int, blend:float)",
        "playAnimByName(name:string, blend:float)",
        "setLooping(loop:bool)",
        "setAnimSpeed(speed:float)  (再生速度倍率。既定1.0、2.0で2倍速、0で一時停止。移動速度と足の同期に)",
        "getAnimCount() -> int",
        "getAnimName(index:int) -> string",
        "-- アニメFSM(.animfsm / AnimatorController)。構造はアセット側、Lua はパラメータだけ触る --",
        "setAnimFloat(name:string, value:float)  (FSM の float パラメータ。例 setAnimFloat(\"speed\", 3.2))",
        "setAnimBool(name:string, value:bool)",
        "setAnimTrigger(name:string)  (1回だけ立つトリガ。条件を満たした遷移が消費する)",
        "getAnimFloat(name:string) -> float  (無ければ 0)",
        "getAnimBool(name:string) -> bool  (無ければ false)",
        "getAnimStateName(layer:int?) -> string  (現在のステート名。layer 省略で 0。無ければ \"\")",
        "getAnimNormalizedTime(layer:int?) -> float  (現ステートの正規化時間 0..1。攻撃の当たり判定窓などに)",
        "playAnimState(stateName:string, blend:float?)  (ステートへ強制遷移。デバッグ/カットシーン用)",
        "setAnimLayerWeight(layer:int, w:float)  (レイヤー重み 0..1。上半身レイヤーのフェードイン等)",
        "getAnimLayerWeight(layer:int) -> float",
        "setFootIKWeight(w:float)  (フット IK の効きを実行時に変える 0..1。FootIK コンポーネントが要る)",
        "getFootIKWeight() -> float",
        "isFootGrounded(rightFoot:bool?) -> bool  (フット IK のレイが地面に当たっているか。省略で左足)",
        "注: アニメイベント(足音)は EventBus に流れる。events:on(\"footstep\", function(ev) ... end) で受ける",
        "-- UI アニメ(.uianim) / スプライトシート(.spranim) --",
        "playUiAnim(clipPath:string?)  (UIAnimPlayer が無ければ付ける。clipPath 省略で現在のクリップを再生)",
        "stopUiAnim()",
        "setUiAnimTime(t:float)  (再生位置を秒で直接指定。スクラブ/ポーズ用)",
        "setUiAnimSpeed(speed:float)",
        "playSprite(seqName:string)  (SpriteAnimator が無ければ付ける。シート内のシーケンス名を再生)",
        "stopSprite()",
        "setSpriteSheet(sheetPath:string)  (assets 相対の .spranim。SpriteAnimator が無ければ付ける)",
        "light() -> Light|nil  (PointLight/DirectionalLight/SpotLight の統一プロキシ。無ければ nil)",
        "addLight(kind:string?) -> Light  (kind: \"point\"(既定)/\"directional\"(\"dir\"/\"sun\")/\"spot\"。既にあればそれを返す)",
        "removeLight()  (付いているライト成分を全部外す。消灯ではなく削除=CB枠が空く)",
        "getFov() / setFov(deg)  (Camera コンポーネントの垂直FOV・度。ズームは毎フレーム絶対値で書く。renderer 側へ書いても翌フレームに戻る)",
    })));
    objects.push_back(O("Light", "entity:light() / entity:addLight(kind) / scene:sun()", json::array({
        "type  (string, read-only: \"point\"/\"directional\"/\"spot\")",
        "id  (u32 entity id, read-only。Flicker のキーに使われる)",
        "isValid() -> bool",
        "intensity  (float, 読み書き)",
        "color  (Vec3, 読み書き。読みは**値コピー**なので from を掴んでも補間中に動かない)",
        "direction  (Vec3, 読み書き。directional/spot のみ。書き込み時に正規化される)",
        "range  (float, 読み書き。point/spot のみ。directional は 0)",
        "ambient  (float, 読み書き。directional のみ = シーン全体の環境光)",
        "innerAngle / outerAngle  (float, 読み書き。spot のみ・度)",
        "castShadows  (bool, 読み書き。point/spot のみ)",
        "setColor(r,g,b) / setDirection(x,y,z)  (Vec3 を作らずに書く近道)",
        "注: 持っていない型のプロパティは「読むと既定値・書くと無視」。型分岐を書かなくていい",
    })));
    objects.push_back(O("transform", "entity.transform / self.transform", json::array({
        "position  (Vec3, 読み書き)", "rotation  (Vec3, euler degrees, 読み書き)", "scale  (Vec3, 読み書き)",
        "代入: tr.position = Vec3.new(x,y,z) も tr.position.x=… も可。tr 自体(entity.transform)の再代入は不可(read-only)",
    })));
    objects.push_back(O("Vec3", "Vec3.new(x,y,z)", json::array({"x", "y", "z"})));
    objects.push_back(O("self", "(各 Lua コンポーネントに自動で渡る)", json::array({
        "entity  (u32 id NUMBER — Entity usertype ではない。callable が要るなら scene:findEntity(self.name))",
        "name  (string)", "transform  (Transform)", "enabled  (bool)",
        "<宣言した properties の各値>  (dx12_get_lua_component_state で確認)",
    })));
    objects.push_back(O("scene", "global", json::array({
        "spawn(name,modelPath,pos,rot,scale) -> entity", "spawnBox(name,pos,rot,scale) -> entity",
        "spawnSphere(name,pos,radius) -> entity", "spawnPlane(name,pos,size,grid) -> entity",
        "remove(entity)", "getEntityCount() -> int",
        // ★見つからなくても nil ではなく「無効な Entity」が返る。`if e then` は常に真になる。
        //   transform を触れば例外で気づけるが、hasComponent は false を返すだけなので黙って外れる。
        "findEntity(name) -> entity  ★見つからなくても nil ではない。必ず e:isValid() で確かめる"
        "（`if e then` は常に true。無効な e への e.transform は例外、e:hasComponent は常に false）",
        "setUVScale(entity,u,v)", "setColor(entity,r,g,b)", "gimmicks() -> table",
        "setSpriteEffect(entity,value)  (Sprite2D.effectValue、カスタムシェーダー用)",
        "setSpriteAlpha(entity,alpha)  (Sprite2D不透明度0..1、半透明演出用)",
        "setSpriteParams(entity,x,y,z,w)  (Sprite2D.shaderParams、カスタムシェーダー汎用float4)",
        "setSpriteUV(entity,u0,v0,u1,v1)  (Sprite2D.uvMin/uvMax 直接指定)",
        "setSpriteScroll(entity,su,sv)  (Sprite2D UVスクロール速度・単位/秒。溶岩/滝等)",
        "setSpriteAnim(entity,frames,fps,cols,row)  (Sprite2D フリップブック。frames=0で停止)",
        "setSpriteAnimMode(entity,mode)  (0=ループ 1=単発 2=往復)",
        "restartSpriteAnim(entity)  (設定は変えず頭から再生し直す)",
        "isSpriteAnimDone(entity) -> bool  (単発の再生完了。爆発スプライトを消すタイミング)",
        "setMeshEffect(entity,value)  (MeshRenderer.effectValue、カスタムシェーダー用)",
        "setMeshParams(entity,x,y,z,w)  (MeshRenderer.shaderParams、カスタムシェーダー汎用float4)",
        "setMeshUvScroll(entity,su,sv)  (MeshRenderer の UV スクロール速度・単位/秒。滝/溶岩/コンベア)",
        "setMeshAnim(entity,frames,fps,cols,row) / setMeshAnimMode(entity,mode) / isMeshAnimDone(entity) -> bool"
        "  (3Dメッシュのフリップブック。Sprite2D 版と同じ流儀)",
        "queryByTag(tag) -> table(names)", "queryInBox(minX,minZ,maxX,maxZ,tag?) -> table(names)",
        "setUiText(e,text) / getUiText(e)  (setはタイプライターを先頭から再生し直す)",
        "setUiTypewriter(e,charsPerSec)  (0=即全表示。UIText.typewriterSpeed)",
        "isUiTypewriterDone(e) -> bool  (会話の「クリックで次へ」判定用)",
        "setUiColor(e,r,g,b,a)", "setUiVisible(e,visible)",
        "setUiTexture(e,path)", "setUiFill(e,amount) / getUiFill(e)  (UIImage.fillAmount 0..1)",
        "setUiUvScroll(e,su,sv)  (UIImage の UV スクロール。流れる背景パターン)",
        "setUiAnim(e,frames,fps,cols,row) / setUiAnimMode(e,mode) / restartUiAnim(e) / isUiAnimDone(e) -> bool"
        "  (UIImage のフリップブック。★entity:playUiAnim/setUiAnimTime は .uianim の別物なので混同しない)",
        "setUiRotation(e,deg) / getUiRotation(e)  (UIRect.rotation 視覚回転・度)",
        "getUiSlider(e) / setUiSlider(e,v)  (UISlider実値。setはonChange発火しない)",
        "getUiToggle(e) / setUiToggle(e,on)  (UIToggle。setはonChange発火しない)",
        "getUiScroll(e) -> x,y / setUiScroll(e,x,y)  (UIScrollViewのスクロール量px)",
        "tweenUi(e,params)  (params: dx,dy,scale,scaleX,scaleY,alpha,rotate(度・絶対値),"
        "color={r,g,b}(乗算・1超え=フラッシュ),shake(px振幅・減衰),shakeFreq,"
        "fill(UIImage.fillAmount絶対値=ゲージなめらか増減),countTo/countFrom/countFmt(UITextへ数字ロール),"
        "onComplete=function()(完了時1回),duration,delay,easing(linear/in/out/inOut/back/bounce/"
        "elastic/expo/inBack/inOutBack/quint/sine))",
        "stopUiTweens(e)  (進行中tween全打ち切り+視覚値リセット。連打対策)",
        "showUi(e)", "hideUi(e)",
        "sun() -> Light|nil  (最初の DirectionalLight。時間帯演出はこれを駆動する)",
        "lightCount() -> { point=, spot=, directional=, total=, maxTotal=1024, maxPerCluster=128 }"
        "  (クラスタードライティング。点/スポットの個別上限は無く合計 1024 灯。1クラスタ 128 灯超は無言で切り捨て)",
        "getAmbient() / setAmbient(v)  (環境光=影の明るさ。実体は DirectionalLight.ambient、書きは全太陽へ)",
        "getShadowsEnabled() / setShadowsEnabled(b)  (リアルタイム影(CSM)の ON/OFF。false で影パスごとスキップ)",
        "getSkybox() -> { envMapPath=, iblIntensity=, skyboxIntensity=, drawSkybox= }",
        "setSkybox{ iblIntensity=, skyboxIntensity=, drawSkybox= }  (渡したキーだけ上書き。envMapPath の実行時差し替えは非対応)",
    })));
    objects.push_back(O("input", "global (':' で呼ぶ)", json::array({
        "isKeyDown(vk) -> bool", "isKeyPressed(vk) -> bool", "isAsyncKeyDown(vk) -> bool",
        "isMouseCaptured() -> bool", "isRightMouseDown() -> bool",
        "getMouseDeltaX() -> float", "getMouseDeltaY() -> float", "setMouseCapture(b)",
        "--- ゲームパッド(XInput。pad = 0..3) ---",
        "isPadConnected(pad) -> bool", "getConnectedPadCount() -> int",
        "isPadButtonDown(pad,button) -> bool / isPadButtonPressed(...) / isPadButtonReleased(...)"
        "  (button は PAD_A/B/X/Y/LB/RB/BACK/START/LSTICK/RSTICK/DPAD_UP/DOWN/LEFT/RIGHT のグローバル定数)",
        "getPadLeftStickX(pad)/getPadLeftStickY(pad)/getPadRightStickX(pad)/getPadRightStickY(pad) -> -1..1"
        "  (デッドゾーン適用済み)",
        "getPadLeftTrigger(pad)/getPadRightTrigger(pad) -> 0..1",
        "setPadVibration(pad,low,high)  (low=強モーター/high=弱モーター。0..1。手動で止める)",
        "setPadVibrationTimed(pad,low,high,sec)  (sec 秒鳴って自動停止)",
        "★prelude に名前で呼べる簡易版がある: padDown(\"A\") / padStick(\"left\") 等（下の prelude 欄）",
        "--- ゲーム UI が入力を食ったか ---",
        "isUiCapturingMouse() -> bool  (カーソルの下に UI がある。暗幕やボタンがクリックを吸う)",
        "isUiCapturingNav() -> bool  (方向/決定入力を UI が使っている＝フォーカスが UI に乗っている)",
        "★エンジンは自動で抑止しない。HUD にボタンを 1 つ置くだけでスティック移動がメニュー移動と"
        "二重に効き、ジャンプ(A/Space)が onClick も撃つので、"
        "ゲーム側で `if input:isUiCapturingNav() then return end` のように自分で止めること",
    })));
    // ★display / net はどちらも「エンジンには完全に実装があるのに describe_lua_api に
    //   1 文字も無い」状態だった。オプション画面もマルチプレイも、ここに載っていなければ
    //   AI からは存在しないのと同じ。
    objects.push_back(O("display", "global (':' で呼ぶ)。映像設定", json::array({
        "setVSync(b) / getVSync() -> bool",
        "setFpsLimit(n) / getFpsLimit() -> int  (0=無制限)",
        "setWindowMode(\"windowed\"|\"borderless\"|\"fullscreen\") / getWindowMode() -> string",
        "setResolution(w,h) / getResolution() -> w,h  (2値を返す)",
        "getResolutions() -> { {w=,h=}, ... }  (選べる解像度の一覧。設定画面のドロップダウン用)",
        "★set 系は即適用＋ settings.json へ保存され、ゲーム起動時に自動で復元される",
    })));
    objects.push_back(O("net", "global (':' で呼ぶ)。マルチプレイ", json::array({
        "host(port?) -> err:string  (\"\"=成功) / join(ip,port?) -> err:string / disconnect()",
        "isServer() / isClient() / isConnected() -> bool", "localClientId() -> int",
        "players() -> { {id=,rtt=,bytesSent=,bytesReceived=}, ... }",
        "setInput{ moveX=,moveZ=,aimYaw=,aimPitch=,buttons=,jump= }  ★クライアント専用・毎フレーム呼ぶ"
        "（呼ばなかったフレームは前回値が送られ続ける）",
        "getInput(entity) -> table  ★サーバー専用。その entity の所有者の最新入力",
        "spawn(prefabPath,x,y,z,owner?) -> netId:int, err:string  ★サーバー専用。netId=0 が失敗。"
        "生成はフレーム境界で走るので完了は net.spawned イベントで受ける",
        "despawn(entity) -> err:string  ★サーバー専用（即時）",
        "findByNetId(netId) -> Entity  ※見つからなくても nil ではない。e:isValid() で確かめる",
        "rpc(name,...) / rpcAll(name,...) / rpcClient(clientId,name,...) / onRpc(name,fn)",
        "★RPC 引数は number/string/boolean/Vec3 のみ。テーブル/関数は**警告なしで nil になる**",
    })));
    objects.push_back(O("camera", "global", json::array({
        "getPosition()/setPosition(v)", "getYaw()/setYaw(f)", "getPitch()/setPitch(f)",
        "moveForward/moveRight/moveUp(amt)", "rotate(dx,dy)",
        // ★この2つを読むのはエディタのフライカメラだけ（Application.cpp の
        //   EngineMode::Editor ブロックと SceneViewPanel）。Play/配布ゲームでは
        //   誰も読まないので、ゲームスクリプトから呼んでも何も起きない。
        "getMoveSpeed/setMoveSpeed  ※エディタのフライ操作専用。ゲーム中は無効",
        "getMouseSensitivity/setMouseSensitivity  ※同上（自作のマウス感度は自前で持つこと）",
        "project(x,y,z) -> (u,v,visible)",
    })));
    objects.push_back(O("physics", "global", json::array({
        "autoCollider(e)", "addBoxCollider(e,hx,hy,hz)", "addSphereCollider(e,radius)",
        "addCapsuleCollider(e,radius,halfHeight)", "addRigidBody(e,motionType,mass)", "removeRigidBody(e)",
        "applyForce(e,vec3)", "applyImpulse(e,vec3)", "setVelocity(e,vec3)", "getVelocity(e) -> vec3",
        "setPosition(e,vec3)  ※DYNAMIC ボディ向け。KINEMATIC/STATIC は Transform 駆動なので entity.transform.position を直接書く",
        "raycast(origin,dir,maxDist) -> RaycastHit  ※戻り値は h.hit / h.distance / h.point / h.normal（() を付けない）",
        "overlapBox(center,half,maxN?) -> {entity..}", "overlapSphere(center,radius,maxN?) -> {entity..}",
        "setGravity(vec3)", "setPaused(b)", "step(dt)",
        "addCharacterController(e,radius,halfHeight)", "move(e,vx,vz)", "jump(e,amount?)", "isGrounded(e) -> bool",
    })));
    objects.push_back(O("nav", "global (':' で呼ぶ)。ナビメッシュ経路探索", json::array({
        "ready() -> bool  (ナビメッシュが焼けているか。エディタの『ツール > ナビメッシュ』か dx12_navmesh_build で焼く)",
        "sample(pos, radius?) -> Vec3|nil  (位置を一番近い歩行面へ落とす。高さは坂道でもボクセル分解能で正確)",
        "findPath(from, to, radius?) -> {Vec3,...}  (A* + ファネル。空テーブルなら経路なし。"
        "最後の点が to から離れていれば『そこまでしか行けない』意味)",
        "raycast(from, to) -> hit:bool, point:Vec3  (壁に当たるまで直進。『経路を張らずに真っ直ぐ行けるか』の判定)",
        "moveAlong(from, to) -> Vec3  (壁で滑らせた移動先。精密な当たり判定つきの1歩)",
        "★findPath は毎フレーム全員ぶん呼ばないこと。数十フレームに 1 回引き直して、間は折れ線を追うだけで足りる",
    })));
    objects.push_back(O("audio", "global", json::array({
        "playBGM(path)/stopBGM()/pauseBGM()/resumeBGM()", "seekBGM(sec)  (再生位置を秒指定でジャンプ。ループ維持、イントロスキップ等)", "setBGMRate(ratio)  (再生速度倍率・ピッチ連動0.05〜2.0。1=通常。playBGMで1.0に戻る)", "setListener(x,y,z)  (空間SFXのリスナー位置上書き。プレイヤー中心の定位に。毎フレーム呼ぶ想定)", "playSFX(path)",
        "playSpatial(path,x,y,z,minD,maxD,vol?,loop?)", "stopAllSFX()",
        "setMasterVolume/setBGMVolume/setSFXVolume(v)",
        "getMasterVolume()/getBGMVolume()/getSFXVolume() -> float  (設定画面のスライダー初期値に要る)",
        "getCurrentBGM() -> string  (今鳴っている BGM の assets 相対パス。鳴っていなければ空)",
        "isBGMPlaying() -> bool",
        "★playBGM は同じパスでも必ず頭出しする。シーンをまたいで同じ曲を流し続けたいときは"
        "getCurrentBGM() で判定して呼ばないこと（曲の途中で遷移するとイントロへ戻る）",
        "getBGMList()/getSFXList() -> table",
        "rescan()  (assets 配下の音声ファイルを列挙し直す。実行中に wav を足したとき用)",
    })));
    objects.push_back(O("time", "global ('.' で呼ぶ)", json::array({
        "time.now() -> float  — Play開始からの経過秒(タイムスケール適用済み)",
        "time.realtime() -> float  — 実時間の経過秒(スケール非適用)",
        "time.dt() -> float / time.realDt() -> float  — 今フレームの dt(スケール済み/実時間)",
        "time.frame() -> int  — フレームカウンタ",
        "time.getScale()/time.setScale(s)  — タイムスケール。0=ポーズ, 0.5=スローモ, 2=早送り。OnUpdate の dt 自体に掛かるので既存スクリプトは無改修で追従(物理/パーティクルは対象外)",
        "time.after(sec, fn) -> id  — sec秒後に fn を1回実行(スケール済み時間で進む)",
        "time.every(sec, fn) -> id  — sec秒ごとに fn を繰り返し実行",
        "time.cancel(id)  — after/every の解除。タイマーは Play 開始でクリア",
        "★actions.bind(name, key, x?, y?, z?)  — 抽象アクションにキーを割り当てる。x,y,z 省略で (1,0,0)＝ボタン用。移動なら方向ベクトルを渡す(例: actions.bind('move', KEY_W, 0,0,1))",
        "actions.get(name) -> x,y,z  — 押されているキーの寄与を合算。actions.down(name) / actions.pressed(name) -> bool（押しっぱなし / このフレームで押した）",
        "actions.clear(name)/clearAll()/count(name)/save()  — 割り当ての解除と保存。save() でプロジェクト直下の input_bindings.json へ書き、次回そのプロジェクトを開いた時に自動復元される",
        "★キーを直接見る(keyDown('W'))のではなく actions を使うと、キーコンフィグを設定画面から差し替えられる。割り当ては Play/Stop をまたいで保持される",
        "★task.spawn(fn, ...) -> id  — fn をコルーチンとして開始。中で wait(sec)/waitFrames(n)/waitUntil(pred) が使える。演出やカットシーン、敵の行動シーケンスは time.after のネストではなくこちらで直線的に書く",
        "task.cancel(id)/task.alive(id)/task.count()/task.cancelAll()  — タスクの中断・生存確認。Play 開始で全クリア",
        "wait(sec)/waitFrames(n)/waitUntil(pred)  — task.spawn の中でだけ呼べる。wait(0) と waitFrames(1) はどちらも次フレーム再開",
        "time.video.start(duration, {skipCost=1.0}?)/stop()/active()  — ステージ共有の\"ビデオ時計\"開始。ギミックは t=video.localTime(self) の純関数で動きを書く(決定論タイムライン)",
        "time.video.now()/duration()/remaining()/finished()  — 動画時間・残り時間(未startなら remaining=math.huge)。skip の消費も残り時間に反映",
        "time.video.skip(entOrName, ±sec) -> offset  — 対象だけ先送り/巻き戻し(オフセット±)。残り時間を |sec|*skipCost 自動消費",
        "time.video.localTime(entOrName) -> t / setOffset/getOffset  — 動画時間+個別オフセット。キーは self テーブル/名前文字列/数値id(名前優先、同名は同一時計)",
        "time.localTime(e)/skipEntity(e,±sec)/scaleEntity(e,s)/getEntityScale(e)/resetEntity(e)  — ビデオ時計と独立したエンティティ個別時計(0=停止、負=逆再生)",
        "charge.new(key, {max=2,rate=1,realtime=false}?) -> c  — 押しっぱなしチャージ計測(弓を引く等)。OnUpdate で c:update()、c:charging()/c:ratio()/c:value()、離した瞬間 c:released() がチャージ量を返す(他は nil)",
    })));
    // ★メソッドではなくプロパティ。sol2 はメンバ変数ポインタ(&RaycastHit::hit 等)を
    //   プロパティとして束縛するので、h:hit() は "attempt to call a boolean value" で落ちる。
    //   ここが () 付きで書いてあったせいで、ドキュメント通りに書くとエラーになっていた。
    objects.push_back(O("RaycastHit", "physics:raycast(...) の戻り値", json::array({
        "★プロパティで読む。h:hit() や h.hit() は実行時エラー（() を付けない）",
        "hit -> bool", "distance -> float", "point -> Vec3", "normal -> Vec3",
        "例: local h = physics:raycast(o, dir, 100); if h.hit then log(h.point.y) end",
        "bodyId は Lua に出していない（当たった相手の Entity は取れない）",
    })));
    objects.push_back(O("ui", "global (':' で呼ぶ)", json::array({
        "ui:text(x,y,text,size?,r?,g?,b?,a?)", "ui:button(x,y,w,h,label) -> bool",
        "ui:image(x,y,w,h,path)", "ui:rect(x,y,w,h,r?,g?,b?,a?,rounding?)",
    })));
    objects.push_back(O("fx", "global (':' で呼ぶ)。座標/色キーは省略可(既定値あり)", json::array({
        "fx:burst{ x,y,z, count, size,sizeEnd, life,lifeVar, r,g,b, rEnd,gEnd,bEnd, rMid,gMid,bMid, intensity, kind, speed,spread, dx,dy,dz, gravity,drag,up, stretch, turbStrength,turbFreq, flicker } — 1発放出",
        "fx:ring{ ...burst と同じキー... } — リング状放出。サイズは radius/scale ではなく size",
        "kind: glow/fire/smoke/spark/magic/electric/ring/star（文字列 or 0..7）",
        "fx:beam{ x0,y0,z0, x1,y1,z1, width, r,g,b, intensity, life, kind } — kind: energy/electric/fire。座標は ax/bz ではなく x0..z1",
        "fx:pulse(amt?)  画面全体パルス  /  fx:clear()",
        "例: fx:burst{ x=p.x, y=p.y, z=p.z, kind=\"spark\", count=18, size=0.5, r=1, g=0.78, b=0.18 }  ← scale/radius は無効キー(黙って無視される)",
    })));
    objects.push_back(O("post / ssao", "global ('.' で呼ぶ)。項目名は MCP の set_post_process と同一", json::array({
        "post.get(name) -> value|nil  /  post.set(name, value) -> bool",
        "post.setMany{ bloomOn=true, bloom=0.8, vignetteOn=true } -> 適用数",
        "post.names() -> table  (使える項目名の一覧。個別バインドは無いのでここで引く)",
        "ssao.get/set/setMany/names も同じ流儀 (enabled,radius,bias,intensity,power,sampleCount,blur)",
        "糖衣: Post.bloom = 0.8 / Ssao.enabled = true（メタテーブル経由。Tween の対象にできる）",
        "Play 中の変更は Stop でシーンJSONごと巻き戻る（エディタの値は壊れない）",
    })));
    objects.push_back(O("events", "global (Play 中のみ)", json::array({
        "events:on(name,fn) -> id", "events:off(id)", "events:emit(name,data?)", "events:clear()",
    })));
    objects.push_back(O("globals", "", json::array({
        "log(...) / logWarn(...) / logError(...) / print(...)  (可変長。tostring でタブ区切り連結。"
        "print は素の Lua print を差し替えたもの＝どれもエディタのコンソールに出る)",
        "saveNum(key,val) / loadNum(key,default?) -> double  ★メモリのみ。シーン間の受け渡し用でアプリ終了で消える",
        "savePersist(key,val) / loadPersist(key,default?) -> double  ★settings.json へディスク永続。"
        "音量・映像設定などはこちら（saveNum と間違えると「設定が保存されない」になる）",
        "PAD_A/PAD_B/PAD_X/PAD_Y/PAD_LB/PAD_RB/PAD_BACK/PAD_START/PAD_LSTICK/PAD_RSTICK/"
        "PAD_DPAD_UP/PAD_DPAD_DOWN/PAD_DPAD_LEFT/PAD_DPAD_RIGHT  (input:isPadButton* に渡す)",
        "loadScene(rel)", "nextScene()", "quit()", "fadeToScene(rel,dur?)",
        "preloadScene(rel)  (次シーンのテクスチャ/モデルを先読み。切替はしない=トランジションのカクつき対策)",
        "transitionToScene(rel,type:int,dur?)  (type: 0=Fade,1=横Wipe,2=Circle,3=縦Wipe,4=シークバー早送り)",
        "setUiFocus(entityOrId)  (フォーカスナビの初期フォーカス。メニュー表示時に既定ボタンへ)",
        "ASSETS, SCREEN_W, SCREEN_H, KEY_*(VK codes), MOTION_STATIC/KINEMATIC/DYNAMIC",
    })));
    objects.push_back(O("prelude", "global (高レベルヘルパ)", json::array({
        "keyDown(name) -> bool / keyPressed(name) -> bool  (name: \"W\",\"SPACE\",\"ESC\" 等)",
        "padConnected(pad?) / padDown(name,pad?) / padPressed(name,pad?) / padReleased(name,pad?)"
        "  (name は \"A\",\"START\",\"DPAD_UP\" 等。PAD_* 定数を書かずに済む)",
        "padStick(\"left\"|\"right\", pad?) -> x,y / padTrigger(\"left\"|\"right\", pad?) -> 0..1",
        "padVibrate(low,high,seconds?,pad?)  (seconds 省略で padVibrate(0,0) するまで鳴り続ける)",
        "actor(name,opts?) -> Actor", "cameraFollow/cameraTPS/cameraLockOn(...)",
        "goToScene(path,dur?)", "win(dur?)", "clamp(v,lo,hi)", "lerp(a,b,t)", "angleDelta(from,to)",
        // ★「FX.explosion/shockwave/spark/...」の「...」は名前が分からず引けないので全部書く。
        "FX.explosion/shockwave/spark/trail/supernova/pillar(x,y,z,...)",
        "FX.hit(amount?)  (画面パルス) / FX.beam(x0,y0,z0,x1,y1,z1,r,g,b,width,kind,intensity)",
        "FX.lightning(x0,y0,z0,x1,y1,z1,r,g,b,width?)",
        "vfx.register(name,fn) / vfx.play(name,x,y,z,scale?)",
        "uifx.punch(e,s?,dur?) / flash(e,r?,g?,b?,dur?) / shake(e,amp?,dur?) / hit(e,amp?) / "
        "bounceIn(e,dur?) / flipIn(e,dur?) / popOut(e,dur?) / fadeIn(e,dur?) / fadeOut(e,dur?)"
        "  (ゲーム内UIの定番演出ワンライナー。e は Entity かボタンイベントの e.source)",
        "uifx.slideInLeft/slideInRight/slideInUp(e,delay?,dist?,dur?) / popIn(e,delay?,dur?)"
        "  (delay 付き＝下の stagger と組み合わせる前提)",
        "uifx.stagger(list, step?, fn, ...)  (list の各要素を step 秒ずつ遅らせて fn に流す。"
        "例: uifx.stagger(items, 0.07, uifx.slideInLeft))",
        "uifx.countTo(e,to,dur?,fmt?)  (数字ロール) / uifx.fillTo(e,v,dur?,easing?)  (ゲージ増減)",
        "uifx.damageBar(front, ghost, v, ghostDelay?)  (格ゲー式。前景バーは即・後追いの薄いバーが遅れて減る)",
        "uifx.wiggle(e,deg?,dur?) / uifx.heartbeat(e,s?,dur?)",
    })));
    // ★actor() は載っていたのにメソッドが 1 つも載っていなかった＝返り値の使い道が分からない状態。
    objects.push_back(O("Actor", "prelude の actor(name,opts?) の戻り値", json::array({
        "Actor:entity() -> Entity|nil  (名前で毎回引き直す。消えていれば nil)",
        "Actor:valid() -> bool",
        "Actor:pos() -> Vec3 / Actor:setPos(x,y,z)",
        "Actor:moveTopDown(dt, \"WASD\"|\"Arrows\")  (見下ろし移動。solid 相手には壁ズリする)",
        "Actor:reached(other, radius?) -> bool  (XZ 距離での到達判定。other も Actor)",
    })));
    objects.push_back(O("Tween / Flicker / Lighting", "global (prelude。ライティング演出レイヤ)", json::array({
        "Tween(target, prop, to, duration, opts?) -> id  — 汎用プロパティ補間。target は table でも "
        "usertype(Light/Transform)でも可。数値と3要素(色/ベクトル)の両方を補間する。"
        "opts: { ease=, delay=, loop=(true|回数), pingpong=, onComplete= }",
        "ease: linear/inQuad/outQuad/inOutQuad/inCubic/outCubic/inOutSine/outBack/outBounce（既定 outQuad）",
        "stopTween(id) / Anim.clear()  — 個別停止 / 全停止（Play 開始で自動クリア）",
        "Flicker(light, style?, hz?) -> light  — Quake 由来 lightstyle 文字列で明滅。1文字=1/10秒、"
        "'a'=消灯 'm'=等倍 'z'≒2.08倍。style はプリセット名か生の文字列",
        "LIGHT_STYLES: normal/candle/fluorescent/broken/pulse/storm/strobe/slowStrobe/gentle",
        "stopFlicker(light)  — 明滅を止めて元の明るさへ戻す（1ライトにつき1つ・掛け直しは上書き）",
        "Lighting.setTimeOfDay(hour) / timeOfDay() / sun()  — 0..24 で太陽の向き/色/強度/環境光を駆動",
        "Lighting.tweenTimeOfDay(hour, duration?, opts?) -> id  — 時刻を補間（既定は最短方向。"
        "opts.forward=true で必ず前進）",
        "Lighting.sample(hour) -> dx,dy,dz,r,g,b,intensity,ambient  — カーブだけ取り出す（自前で使う用）",
        "Lighting.dayColor/duskColor/nightColor/dayIntensity/nightIntensity/dayAmbient/nightAmbient/duskAmbient"
        "  — 作風を変える調整ノブ（テーブルを書き換えるだけ）",
        "Lighting.lightningFlash{ power=6, color={r,g,b}, times=2, gap=0.09, dur=0.06 }  — 雷の閃光",
        "Lighting.fadeToBlack(sec?, onDone?) / fadeFromBlack(sec?, onDone?)  — 露出で暗転/復帰",
        "Lighting.pulse(light, hz?, min?, max?) -> id  — min..max を往復（呼吸/鼓動）",
        "Lighting.tweenColor(light, r,g,b, dur?, opts?) / tweenIntensity(light, v, dur?, opts?)",
        "findLight(nameOrEntityOrSelf) -> Light|nil  — 名前/Entity/self からライトを引く近道",
        "注: 演出は既存の毎フレームフック(__time_tick)で駆動＝time.setScale(0) で一緒に止まる",
    })));
    return json{
        {"version", 1},
        {"note", "Lua コンポーネントから使えるバインディング一覧。重要: コンポーネントは transform を除き "
                 "entity.<key> では読めない(entity.boxCollider 等は nil)。collider/rigidBody の値は "
                 "physics:getVelocity(e) など別 API 経由。self.entity は数値 id で Entity usertype ではない。"
                 " コールバックは OnStart(self) / OnUpdate(self, dt)(コンポーネントは self 必須)。"
                 " 位置更新は entity.transform.position = Vec3.new(x,y,z)（KINEMATIC も Transform 駆動）。"
                 " スクリプトエラーは dx12_get_lua_component_state の errorMessage に出る(loadError=true のとき)。"},
        {"objects", std::move(objects)},
    };
}

bool WriteBgraPng(const std::wstring& path, const uint8_t* bgra,
                         uint32_t w, uint32_t h, std::string& err)
{
    using Microsoft::WRL::ComPtr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // 既初期化なら S_FALSE。Uninit はしない(常駐エディタで無害)。

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
    { err = "WIC factory failed"; return false; }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
    { err = "WIC stream open failed"; return false; }

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
    { err = "WIC encoder init failed"; return false; }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2>         props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)) ||
        FAILED(frame->Initialize(props.Get())))
    { err = "WIC frame init failed"; return false; }

    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;   // PNG エンコーダがネイティブ対応＝変換なし
    if (FAILED(frame->SetSize(w, h)) || FAILED(frame->SetPixelFormat(&fmt)))
    { err = "WIC frame setup failed"; return false; }

    const UINT stride = w * 4;
    if (FAILED(frame->WritePixels(h, stride, stride * h, const_cast<BYTE*>(bgra))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    { err = "WIC write failed"; return false; }

    return true;
}

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002   // Win8.1+ SDK。DX スワップチェイン内容も含めて描かせる
#endif

// エディタウィンドウのクライアント領域全体(ImGui パネル込み)を PNG へ書く。
// scene RT に乗らない UI エディタ/ゲーム内 UI プレビュー等を AI が「見る」ための経路。
// PrintWindow(PW_RENDERFULLCONTENT) はウィンドウが他窓の背後でも正しく描かせられる。
// 最小化中はサイズが取れないため失敗を返す(呼び出し元がエラーメッセージで案内)。
std::string CaptureWindowScreenshot(HWND hwnd, std::string& err)
{
    namespace fs = std::filesystem;
    if (!hwnd || IsIconic(hwnd)) { err = "window is minimized (restore the editor window first)"; return {}; }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) { err = "window size is 0"; return {}; }

    HDC wdc = GetDC(hwnd);
    HDC mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    std::string result;
    if (dib && bits)
    {
        HGDIOBJ old = SelectObject(mdc, dib);
        BOOL ok = PrintWindow(hwnd, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        if (!ok)   // 古い環境向けフォールバック(前面にある時だけ正しい絵になる)
            ok = BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY);
        if (ok)
        {
            // GDI の DIB は alpha 未定義(0)のことがある → PNG が全透明にならないよう 255 で埋める
            auto* px = static_cast<uint8_t*>(bits);
            for (int i = 0; i < w * h; ++i) px[i * 4 + 3] = 0xFF;
            const fs::path outPath = fs::absolute("mcp_ui_screenshot.png");
            if (WriteBgraPng(outPath.wstring(), px, static_cast<uint32_t>(w),
                             static_cast<uint32_t>(h), err))
                result = outPath.string();
        }
        else
        {
            err = "PrintWindow/BitBlt failed";
        }
        SelectObject(mdc, old);
    }
    else
    {
        err = "DIB alloc failed";
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    return result;
}


} // namespace appdetail

} // namespace dx12e
