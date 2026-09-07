#pragma once
// ===========================================================================
// Application 実装 TU 共通の内部ヘッダ（Application.cpp 分割の受け皿）
// ---------------------------------------------------------------------------
// Application の実装は 1 ファイル 18k 行のゴッドファイルだった。**Application.h も
// クラス定義も一切変えずに**、メンバ関数の定義をテーマ別の TU へ機械分割してある:
//   Application.cpp            ctor / Initialize / Run / Update / Shutdown
//   ApplicationPipeline.cpp    PSO 再生成 / レンダー解像度 / PSO・SRV キャッシュ
//   ApplicationRender.cpp      Render() と描画の下請け一式
//   ApplicationScene.cpp       シーンロード / Play⇔Editor 遷移 / 永続化 / スカイボックス
//   ApplicationProject.cpp     プロジェクト / バージョン管理 / ゲームビルド
//   mcp/ApplicationMcp*.cpp    MCP ディスパッチ表（テーマ別に 8 ファイル）
//
// ★狙いは「別々の機能を別々の担当が同時に触れること」。新しい実装は該当テーマの
//   ファイルへ足すこと。共通ヘルパでない限りここへ戻さない（また肥大化する）。
//
// このヘッダが持つのは複数 TU から参照される元・ファイルローカルなヘルパだけで、
// 旧 Application.cpp の無名名前空間を dx12e::appdetail へ移しただけ（中身は無改造）。
// 各実装 TU は先頭で `using namespace appdetail;` するので呼び出し表記は変わらない。
// ===========================================================================

#include "Application.h"
#include "Logger.h"
#include "Assert.h"
#include "PathResolver.h"
#include "Version.h"

// Graphics module headers
#include "graphics/GraphicsDevice.h"
#include "graphics/CommandQueue.h"
#include "graphics/DeferredRelease.h"
#include "graphics/SwapChain.h"
#include "graphics/FrameResources.h"
#include "core/SplashScreen.h"
#include "graphics/DescriptorHeap.h"
#include "graphics/GpuResource.h"
#include "graphics/Buffer.h"
#include "graphics/RootSignature.h"
#include "graphics/PipelineState.h"
#include "graphics/CommandList.h"
#include "graphics/Texture.h"
#include "graphics/RenderTarget.h"
#include "graphics/GpuTimer.h"
#include "renderer/Mesh.h"
#include "renderer/Material.h"
#include "renderer/Camera.h"
#include "renderer/Frustum.h"
#include "renderer/PostProcess.h"
#include "renderer/ScreenShaderPass.h"
#include "renderer/BloomPass.h"
#include "renderer/AutoExposurePass.h"
#include "renderer/GodRaysPass.h"
#include "renderer/LensFlarePass.h"
#include "renderer/DofPass.h"
#include "renderer/MotionBlurPass.h"
#include "renderer/GpuParticleSystem.h"
#include "renderer/SSAOPass.h"
#include "renderer/HiZPass.h"
#include "renderer/OcclusionCullPass.h"
#include "renderer/ContactShadowPass.h"
#include "renderer/TaaPass.h"
#include "renderer/RenderDebugPass.h"
#include "renderer/ScreenSpaceGiPass.h"
#include "renderer/RaytracingScene.h"
#include "renderer/RtScreenPass.h"
#include "renderer/SkinningCompute.h"
#include "renderer/DdgiVolume.h"
#include "renderer/VolumetricFogPass.h"
#include "renderer/DecalSystem.h"
#include "renderer/PrevWorldComponent.h"
#include "renderer/ParticleSystem.h"
#include "renderer/SpriteRenderer.h"
#include "renderer/SpriteAnim.h"
#include "renderer/SceneTransition.h"
#include "renderer/IBLBaker.h"
#include "renderer/SkyboxRenderer.h"
#include "resource/ShaderCompiler.h"
#include "resource/ShaderManager.h"
#include "resource/ShaderParams.h"
#include "resource/ModelLoader.h"
#include "resource/ResourceManager.h"
#include "resource/TextureLoader.h"
#include "resource/MaterialAssetManager.h"
#include "resource/TerrainLayerSet.h"
#include "graphics/Texture.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/SkinningBuffer.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "input/InputSystem.h"
#include "scene/Scene.h"
#include "scene/SceneSettingsHash.h"
#include "scene/Entity.h"
#include "ecs/Components.h"
#include "scripting/ScriptEngine.h"
#include "ui/UISystem.h"
#include "ui/UiAnimRuntime.h"
#include "animation/AnimGraphRuntime.h"
#include "animation/FootIK.h"
#include "core/mcp/McpBridge.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>    // MCP read_lua_component / validate_scene のレポート読み込み用
#include <cstdio>     // sscanf_s（ahead/behind 解析）
#include <cctype>     // std::isalnum（ビルド出力フォルダ名のサニタイズ）
#include <cmath>      // sin/cos/atan2/asin（カメラのワールド変換→yaw/pitch 逆算）
#include <map>        // 変更ファイルツリーの構築
#include <chrono>     // 段階的シーンロードの時間予算
#include <unordered_set>  // 先読みアセットの重複除去
#include "audio/AudioSystem.h"
#include "physics/PhysicsSystem.h"
#include "network/NetworkSystem.h"
#include "network/NetworkConfig.h"
#include "physics/PhysicsDebugRenderer.h"
#include "gui/ImGuiManager.h"
#include "gui/UiTestHarness.h"
#include "scene/SceneSerializer.h"
#include "scene/SceneFlow.h"
#include "engine/ecs/ComponentRegistry.h"   // MCP set_component の deserialize 走査用
#include "editor/EditorContext.h"
#include "editor/EditorLayer.h"
#include "editor/EditorTheme.h"        // バージョン管理パネルのステータス配色
#include "core/Version.h"              // kEngineVersion / 「更新内容」ポップアップの中身
#include "core/AutosaveSlot.h"         // .autosave/ スロットの読み取りと破棄
#include "editor/EditorIconRenderer.h"
#include "editor/UndoSystem.h"
#include "editor/ModelThumbnailRenderer.h"
#include "editor/panels/McpBridgePanel.h"
#include "editor/panels/NetworkPanel.h"
#include "editor/panels/VfxEditorPanel.h"
#include "editor/panels/UiEditorPanel.h"
#include "editor/panels/AnimationEditorPanel.h"
#include "editor/panels/SpriteSheetEditorPanel.h"
#include "editor/panels/MaterialEditorPanel.h"
#include "editor/panels/MaterialLibraryPanel.h"
#include "editor/panels/TerrainPanel.h"   // 地形ツール（状態は関数ローカル static なので所有しない）
#include "editor/panels/NavMeshPanel.h"   // ナビメッシュ窓（同上。MCP からも BuildForScene を呼ぶ）
#include "editor/panels/SculptPanel.h"    // スカルプト窓（同上。ハイトフィールドで作れん異形の担当）
#include "editor/ScenePick.h"             // MCP dx12_pick / raycast_precise / snap_to_ground（エディタと同じ実装）
#include "editor/LightingPresets.h"       // MCP dx12_apply_lighting_preset（エディタの窓と同じ表）
#include "terrain/HeightField.h"          // MCP dx12_terrain_*
#include "terrain/TerrainBrush.h"
#include "terrain/TerrainIO.h"
#include "terrain/SculptMesh.h"           // MCP dx12_sculpt_*
#include "terrain/SculptIO.h"
#include "gui/DeepDiagnostics.h"          // MCP dx12_diagnose（機械可読な一括診断）
#include "project/Project.h"
#include "project/ProjectManager.h"
#include "project/GitIntegration.h"
#include "vfs/Vfs.h"
#include "vfs/PakWriter.h"
#include <commdlg.h>
#include <shellapi.h>   // ShellExecuteA（ビルド完了後にフォルダを開く）

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include "gui/ImGuizmo.h"

#include <directx/d3d12.h>
#include <DirectXMath.h>
#include <filesystem>
#include <thread>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <cctype>
#include <unordered_map>
#include <immintrin.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <wincodec.h>                 // MCP screenshot: OS 標準 WIC で PNG 書き出し(外部依存なし)
#include <DirectXPackedVector.h>      // XMConvertHalfToFloat(FP16 sceneRT → 8bit)
#pragma comment(lib, "windowscodecs.lib")


namespace dx12e
{
namespace appdetail
{

// オフスクリーンのシーンカラーは HDR(FP16) で描く。発光が 1.0 を超えられるので
// ブルームとトーンマップで「白熱して光る」パーティクル/エフェクトが出せる。
// scene RT / cameraPreview RT、およびそこへ描く 3D PSO 群はこの形式で揃える。
// （バックバッファ＝スワップチェインは従来どおりスワップ形式のまま）
static constexpr DXGI_FORMAT kSceneColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// G-Buffer（深度+速度プリパスの RTV1）。xy=oct(ワールド法線) z=roughness w=metallic。
// 8bit だとカメラが動くたびに specular がウォブルするので fp16。
// ScreenSpaceGiPass::kGBufferFormat と同じ値であること。
static constexpr DXGI_FORMAT kGBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

// フルパスを assets ディレクトリ相対へ（シーンフロー / loadScene 用）
inline std::string ToAssetRel(const std::string& full)
{
    auto norm = [](std::string s) { for (auto& c : s) if (c == '\\') c = '/'; return s; };
    std::string f = norm(full);
    std::string base = norm(PathResolver::AssetsDir());
    if (!base.empty() && f.rfind(base, 0) == 0)
        return f.substr(base.size());
    return f;
}

// ---- 「更新内容」ポップアップの表示済み版マーカー ----
// UTF-8 → wstring 変換は PathResolver::Utf8ToWide に一本化した
// （マージ前は両ブランチが同じ修正を別実装で持っていた）。

// 「更新内容」ポップアップを表示済みのバージョンを記録するファイル。
// %LOCALAPPDATA%\DX12Engine\shown_version.txt（exe の場所に依存せず必ず書ける）。
// Updater の last_update.txt と同じ場所に置く。
inline std::filesystem::path WhatsNewStatePath()
{
    char* base = nullptr;
    size_t len = 0;
    if (_dupenv_s(&base, &len, "LOCALAPPDATA") != 0 || !base) return {};
    std::filesystem::path dir = std::filesystem::path(base) / "DX12Engine";
    free(base);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "shown_version.txt";
}

inline std::string ReadShownVersion()
{
    std::filesystem::path p = WhatsNewStatePath();
    if (p.empty()) return {};
    std::ifstream f(p);
    if (!f) return {};
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

inline void WriteShownVersion(const std::string& v)
{
    std::filesystem::path p = WhatsNewStatePath();
    if (p.empty()) return;
    std::ofstream f(p, std::ios::trunc);
    if (f) f << v;
}

// MCP set_component / remove_component 共有の小スイッチ。
// jsonKey(get_entity が返すキー = deserialize が見るキー) を対応する型へ写して reg.remove<T>(e)。
// SceneSerializer の RegisterCoreComponentSerializers 登録済みコア部品に限定。
// 未対応キーは false(呼び側が error を返す)。meshRenderer はメッシュ所有整合が要るため非対応。
inline bool RemoveRegisteredComponent(entt::registry& reg, entt::entity e, const std::string& key)
{
    if      (key == "pointLight")          reg.remove<PointLight>(e);
    else if (key == "directionalLight")    reg.remove<DirectionalLight>(e);
    else if (key == "spotLight")           reg.remove<SpotLight>(e);
    else if (key == "camera")              reg.remove<CameraComponent>(e);
    else if (key == "rigidBody")           reg.remove<RigidBody>(e);
    else if (key == "boxCollider")         reg.remove<BoxCollider>(e);
    else if (key == "sphereCollider")      reg.remove<SphereCollider>(e);
    else if (key == "capsuleCollider")     reg.remove<CapsuleCollider>(e);
    else if (key == "characterController") reg.remove<CharacterController>(e);
    else if (key == "tags")                reg.remove<Tag>(e);
    else if (key == "data")                reg.remove<DataComponent>(e);
    else if (key == "sprite2d")            reg.remove<Sprite2D>(e);
    else if (key == "audioSource")         reg.remove<AudioSource>(e);
    else if (key == "particleEmitter")     reg.remove<ParticleEmitter>(e);
    else if (key == "trigger")             reg.remove<Trigger>(e);
    else if (key == "gimmick")             reg.remove<Gimmick>(e);
    else if (key == "convexHullCollider")  reg.remove<ConvexHullCollider>(e);
    else if (key == "luaScript")           reg.remove<LuaScript>(e);
    else if (key == "trailRenderer")       reg.remove<TrailRenderer>(e);
    else if (key == "decal")               reg.remove<DecalComponent>(e);
    else if (key == "networkIdentity")     reg.remove<NetworkIdentity>(e);
    else if (key == "networkTransform")    reg.remove<NetworkTransform>(e);
    else if (key == "uiCanvas")            reg.remove<UICanvas>(e);
    else if (key == "uiRect")              reg.remove<UIRect>(e);
    else if (key == "uiImage")             reg.remove<UIImage>(e);
    else if (key == "uiText")              reg.remove<UIText>(e);
    else if (key == "uiButton")            reg.remove<UIButton>(e);
    else if (key == "uiSlider")            reg.remove<UISlider>(e);
    else if (key == "uiToggle")            reg.remove<UIToggle>(e);
    else if (key == "uiScrollView")        reg.remove<UIScrollView>(e);
    else if (key == "uiLayout")            reg.remove<UILayout>(e);
    else if (key == "uiAnimator")          reg.remove<UIAnimator>(e);
    else if (key == "uiAnimPlayer")        reg.remove<UIAnimPlayer>(e);
    else if (key == "spriteAnimator")      reg.remove<SpriteAnimator>(e);
    else if (key == "animatorController")  reg.remove<AnimatorController>(e);
    else if (key == "footIK")              reg.remove<FootIK>(e);
    else return false;
    return true;
}

// float3 を JSON 配列から読む小ヘルパ(SceneSerializer の DeserializeFloat3 相当・Application 内版)。
inline DirectX::XMFLOAT3 McpF3(const nlohmann::json& j, DirectX::XMFLOAT3 def = {0.0f, 0.0f, 0.0f})
{
    if (j.is_array() && j.size() >= 3)
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
    return def;
}

// レジストリ未登録(orphan)コンポーネントを set_component から適用する。
// SceneSerializer の instantiate 側 deserialize と同じキー/既定値で emplace_or_replace するので
// save/load 経路には一切触れない(=シリアライズ回帰リスクゼロ)。対応外キーは false。
inline bool ApplyOrphanComponent(entt::registry& reg, entt::entity e,
                          const std::string& comp, const nlohmann::json& d)
{
    if (comp == "gimmick")
    {
        // ★部分更新: 既存があればその写しから始める（未指定キーを既定値へ戻さない）
        Gimmick gm = reg.all_of<Gimmick>(e) ? reg.get<Gimmick>(e) : Gimmick{};
        gm.kind = d.value("kind", gm.kind); gm.period = d.value("period", gm.period);
        gm.phase = d.value("phase", gm.phase); gm.amplitude = d.value("amplitude", gm.amplitude);
        gm.threshold = d.value("threshold", gm.threshold); gm.solid = d.value("solid", gm.solid);
        gm.deadly = d.value("deadly", gm.deadly);
        reg.emplace_or_replace<Gimmick>(e, gm);
        return true;
    }
    if (comp == "audioSource")
    {
        // ★部分更新: 既存があればその写しから始める（未指定キーを既定値へ戻さない）
        AudioSource as = reg.all_of<AudioSource>(e) ? reg.get<AudioSource>(e) : AudioSource{};
        as.clipPath = d.value("clipPath", as.clipPath); as.volume = d.value("volume", as.volume);
        as.loop = d.value("loop", as.loop); as.spatial = d.value("spatial", as.spatial);
        as.playOnStart = d.value("playOnStart", as.playOnStart);
        as.minDistance = d.value("minDistance", as.minDistance); as.maxDistance = d.value("maxDistance", as.maxDistance);
        reg.emplace_or_replace<AudioSource>(e, std::move(as));
        return true;
    }
    if (comp == "particleEmitter")
    {
        // ★部分更新: 既存があればその写しから始める（未指定キーを既定値へ戻さない）
        ParticleEmitter emitter = reg.all_of<ParticleEmitter>(e) ? reg.get<ParticleEmitter>(e) : ParticleEmitter{};
        // レイヤー化以降も、この経路は従来どおり「1 枚目を編集する」意味にしておく
        // （既存の set_component 呼び出しをそのまま動かすため）。
        // 別のレイヤーを触りたい場合は "layer" にレイヤー名か添字を渡す。
        if (emitter.layers.empty()) emitter.layers.emplace_back();
        ParticleLayer* target = nullptr;
        if (d.contains("layer"))
        {
            if (d["layer"].is_number_integer())
            {
                const int idx = d["layer"].get<int>();
                if (idx >= 0 && idx < static_cast<int>(emitter.layers.size()))
                    target = &emitter.layers[static_cast<size_t>(idx)];
            }
            else if (d["layer"].is_string())
            {
                target = emitter.FindLayer(d["layer"].get<std::string>());
            }
            if (!target) return false;   // 指定したレイヤーが無い＝黙って 1 枚目を触らない
        }
        else
        {
            target = &emitter.layers[0];
        }
        ParticleLayer& pe = *target;
        pe.name = d.value("name", pe.name);
        if (d.contains("offset")) pe.offset = McpF3(d["offset"], {0.0f, 0.0f, 0.0f});
        pe.kind = d.value("kind", pe.kind); pe.blend = d.value("blend", pe.blend); pe.rate = d.value("rate", pe.rate);
        pe.orient = d.value("orient", pe.orient);   // ★抜けていた（get_entity には出るのに書けなかった）
        pe.playOnStart = d.value("playOnStart", pe.playOnStart); pe.looping = d.value("looping", pe.looping);
        pe.duration = d.value("duration", pe.duration);
        if (d.contains("dir")) pe.dir = McpF3(d["dir"], {0.0f, 1.0f, 0.0f});
        pe.spread = d.value("spread", pe.spread); pe.speed = d.value("speed", pe.speed);
        pe.speedVar = d.value("speedVar", pe.speedVar); pe.size = d.value("size", pe.size);
        pe.sizeEnd = d.value("sizeEnd", pe.sizeEnd); pe.life = d.value("life", pe.life);
        pe.lifeVar = d.value("lifeVar", pe.lifeVar);
        if (d.contains("color"))    pe.color    = McpF3(d["color"], {1.0f, 0.6f, 0.2f});
        if (d.contains("colorEnd")) pe.colorEnd = McpF3(d["colorEnd"], {1.0f, 0.12f, 0.05f});
        if (d.contains("colorMid")) { pe.colorMid = McpF3(d["colorMid"], {1.0f, 0.6f, 0.2f}); pe.hasColorMid = true; }
        pe.hasColorMid = d.value("hasColorMid", pe.hasColorMid);
        pe.intensity = d.value("intensity", pe.intensity); pe.gravity = d.value("gravity", pe.gravity);
        pe.drag = d.value("drag", pe.drag); pe.up = d.value("up", pe.up); pe.stretch = d.value("stretch", pe.stretch);
        pe.turbStrength = d.value("turbStrength", pe.turbStrength); pe.turbFreq = d.value("turbFreq", pe.turbFreq);
        pe.sizeMid = d.value("sizeMid", pe.sizeMid); pe.distort = d.value("distort", pe.distort);
        pe.light = d.value("light", pe.light); pe.lightRange = d.value("lightRange", pe.lightRange);
        pe.flicker = d.value("flicker", pe.flicker); pe.flickerFreq = d.value("flickerFreq", pe.flickerFreq);
        pe.gpu = d.value("gpu", pe.gpu);
        pe.texturePath = d.value("texturePath", pe.texturePath);
        pe.shaderPath  = d.value("shaderPath", pe.shaderPath);
        reg.emplace_or_replace<ParticleEmitter>(e, emitter);
        return true;
    }
    if (comp == "trigger")
    {
        // ★部分更新: 既存があればその写しから始める（未指定キーを既定値へ戻さない）
        Trigger tr = reg.all_of<Trigger>(e) ? reg.get<Trigger>(e) : Trigger{};
        tr.shape = d.value("shape", tr.shape);
        if (d.contains("halfExtents")) tr.halfExtents = McpF3(d["halfExtents"], {1.0f, 1.0f, 1.0f});
        tr.radius = d.value("radius", tr.radius);
        if (d.contains("offset")) tr.offset = McpF3(d["offset"]);
        tr.filter = d.value("filter", tr.filter); tr.once = d.value("once", tr.once);
        // guid を明示できる。省略時は 0 のままで、名前で解決され、読み込み時に昇格する
        // （＝名前だけ指定する従来の呼び方はそのまま動く）。
        tr.filterGuid = ParseEntityGuidHex(d.value("filterGuid", std::string{}));
        if (d.contains("actions") && d["actions"].is_array())
        {
            for (const auto& aj : d["actions"])
            {
                TriggerAction a;
                a.when = aj.value("when", 0); a.type = aj.value("type", 0);
                a.target = aj.value("target", std::string{}); a.str = aj.value("str", std::string{});
                a.targetGuid = ParseEntityGuidHex(aj.value("targetGuid", std::string{}));
                a.num = aj.value("num", 0.0);
                if (aj.contains("vec")) a.vec = McpF3(aj["vec"]);
                tr.actions.push_back(std::move(a));
            }
        }
        reg.emplace_or_replace<Trigger>(e, std::move(tr));
        return true;
    }
    return false;
}

// MCP エラーコード（Node 側が JSON-RPC コードへ写像し、AI が分類/回復に使う）。
namespace McpErr
{
    constexpr int InvalidParam     = 2;  // 引数不正（既定: 検証エラーはこれ）
    constexpr int ModeConflict     = 3;  // Editor/Playing が要件と合わない
    constexpr int StaleScene       = 4;  // sceneGeneration 不一致（再読込後の古い id）
    constexpr int NotFound         = 1;  // entity / scene / asset が無い
    constexpr int UnknownComponent = 6;  // 未対応コンポーネント jsonKey
    constexpr int Internal         = 7;  // エンジン内部エラー
}

// error_code を運べる例外。HandleMcpCommand の catch で resp へ写す。
//
// hint / validValues は「AI の次の一手」を確定させるための追加情報（任意）。
// 応答の error_hint / error_values に載り、Node 側が Error.hint / Error.valid_values として
// 受け取ってツールのエラーメッセージに整形する。付いていない旧来のエラーはそのまま動く。
struct McpError : std::runtime_error
{
    int                      code;
    std::string              hint;         // 「次にどうすればいいか」を必ず 1 文で
    std::vector<std::string> validValues;  // 列挙型の引数なら有効値を全部返す（推測させない）

    McpError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
    McpError(int c, const std::string& m, std::string h, std::vector<std::string> v = {})
        : std::runtime_error(m), code(c), hint(std::move(h)), validValues(std::move(v)) {}
};

// ---- perf_stats / benchmark 共通の集計・整形 ----
// 平均値を詰めた PerfSummary を JSON レポート（数値 + 簡易ボトルネック解析）へ変換する。
// 解析はヒューリスティック: フレーム時間に対する GPU 合計・CPU 実働・待ち時間の比率で分類。
inline const char* CpuScopeName(u32 i)
{
    switch (i)
    {
    case CpuUpdate:     return "update";      // スクリプト/物理/アニメ/エディタ入力
    case CpuBuildList:  return "buildList";   // BuildDrawList の走査部（ワールド行列・LOD・鍵計算）
    case CpuListSort:   return "listSort";    // 描画リストのソート（buildList の内数ではなく別計上）
    case CpuShadowRec:  return "shadowRec";   // CSM 4カスケードのコマンド記録
    case CpuMainRec:    return "mainRec";     // メインパスのコマンド記録
    case CpuEditorUi:   return "editorUi";    // ImGui エディタUI構築（ゲームでは 0）
    // picking / gizmo は editorUi の「内訳」。editorUi にも同じ時間が入る二重計上だが、
    // 「エディタUIが重い」の中身がピッキングなのかギズモなのかを名指しするのが目的。
    case CpuPicking:    return "picking";     // シーンビューのレイキャスト選択（editorUi の内数）
    case CpuGizmo:      return "gizmo";       // ImGuizmo の操作・描画（editorUi の内数）
    case CpuLights:     return "lights";      // ライト収集（ECS 全走査＋ワールド行列）とクラスタへの転送
    case CpuPrepass:    return "prepass";     // 深度プリパス / SSAO / SSR / SSGI / RT の記録
    case CpuImGui:      return "imgui";       // ImGui の描画データ生成と記録（editorUi とは別。ゲーム内 UI も含む）
    case CpuMcp:        return "mcp";         // MCP コマンドの処理（AI が叩いている間だけ増える）
    default:            return "?";
    }
}

struct PerfSummary
{
    double fps = 0, frameMs = 0, frameMsMin = 0, frameMsMax = 0, frameMsP95 = 0;
    double workMs = 0, fenceWaitMs = 0, presentMs = 0;   // 平均
    double draws = 0, culled = 0, tris = 0;              // 平均
    double gpuMs[GpuTimer::Count] = {};                  // 平均
    double cpuMs[CpuScopeCount] = {};                    // 平均（workMs の内訳）
    int    samples = 0;
};

// v を昇順ソートして p(0..1) 分位点を返す（サンプル少数でも落ちない素朴実装）。
inline double PerfPercentile(std::vector<f32>& v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = (std::min)(v.size() - 1,
        static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
    return v[i];
}

nlohmann::json PerfReportJson(const PerfSummary& s, bool vsync, float fpsLimit);

// MCP のエンティティ指定を解決する。params["name"](完全一致 FindEntity) を優先し、
// 無ければ params["entity"](数値 id) を検証して返す。どちらも解決できなければ NotFound を投げる。
// ※ コンポーネント有無は見ない(呼び出し側で all_of を別に確認 → エラー文を分けるため)。
inline entt::entity ResolveMcpEntity(Scene& scene, const nlohmann::json& params)
{
    auto it = params.find("name");
    if (it != params.end() && it->is_string())
    {
        auto ent = scene.FindEntity(it->get<std::string>());
        if (ent.IsValid()) return ent.GetHandle();
        throw McpError(McpErr::NotFound, "no entity named '" + it->get<std::string>() + "'");
    }
    auto e = static_cast<entt::entity>(params.value("entity", 0xFFFFFFFFu));
    if (scene.GetRegistry().valid(e)) return e;
    // 数値 id が無効: Stop/open_scene で世代が変わると古い id はここに来る。再取得を促す。
    throw McpError(McpErr::NotFound,
        "invalid entity id (Stop/シーン再読込で id は変わる。dx12_list_entities で取り直すか name 指定で操作してくれ)");
}

// MCP パス系ツール共通: assets 相対パスの検証。絶対パス/ドライブレター/バックスラッシュ/".."
// を拒否して assets ルート外へのアクセス(traversal)を防ぐ。通れば rel をそのまま返す。
inline std::string ValidateMcpAssetRelPath(const std::string& rel, const char* paramName = "path")
{
    if (rel.empty())
        throw McpError(McpErr::InvalidParam, std::string("missing '") + paramName + "'");
    if (rel.front() == '/' || rel.find('\\') != std::string::npos ||
        rel.find(':') != std::string::npos || rel.find("..") != std::string::npos)
        throw McpError(McpErr::InvalidParam, std::string("invalid ") + paramName + " (assets 相対のみ)");
    return rel;
}

// MCP get_bounds / snap_to_ground 用: エンティティ単体のワールド AABB。
// メッシュ AABB の 8 頂点をワールド行列で変換して包む(回転にも正しい)。
// メッシュ無し(ライト/カメラ/empty)はワールド位置の点(min=max)。Transform 無しは false。
inline bool McpWorldAabb(const entt::registry& reg, entt::entity e,
                  DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax, bool& outHasMesh)
{
    using namespace DirectX;
    if (!reg.all_of<Transform>(e)) return false;
    const XMMATRIX world = ComputeWorldMatrix(reg, e);
    XMVECTOR mn = XMVectorReplicate(3.402823466e+38f);
    XMVECTOR mx = XMVectorReplicate(-3.402823466e+38f);
    outHasMesh = false;
    if (reg.all_of<MeshRenderer>(e))
    {
        for (const auto* mesh : reg.get<MeshRenderer>(e).meshes)
        {
            if (!mesh) continue;
            const XMFLOAT3 amn = mesh->GetAABBMin();
            const XMFLOAT3 amx = mesh->GetAABBMax();
            for (int c = 0; c < 8; ++c)
            {
                XMVECTOR p = XMVectorSet((c & 1) ? amx.x : amn.x,
                                         (c & 2) ? amx.y : amn.y,
                                         (c & 4) ? amx.z : amn.z, 1.0f);
                p = XMVector3Transform(p, world);
                mn = XMVectorMin(mn, p);
                mx = XMVectorMax(mx, p);
            }
            outHasMesh = true;
        }
    }
    if (!outHasMesh)
    {
        const XMVECTOR p = XMVectorSetW(world.r[3], 1.0f);
        mn = p; mx = p;
    }
    XMStoreFloat3(&outMin, mn);
    XMStoreFloat3(&outMax, mx);
    return true;
}

// child の親チェーンに ancestor が居るか(child==ancestor も true)。循環ガード付き。
inline bool McpIsDescendantOf(const entt::registry& reg, entt::entity child, entt::entity ancestor)
{
    entt::entity cur = child;
    int depth = 0;
    while (cur != entt::null && reg.valid(cur) && depth++ < 64)
    {
        if (cur == ancestor) return true;
        if (!reg.all_of<Transform>(cur)) break;
        cur = reg.get<Transform>(cur).parent;
    }
    return false;
}

// MCP key_* 用。params["key"] を Win32 VK コードに解決する。数値(VK そのもの)か、
// 文字列(1文字 A-Z/0-9 or "SPACE"/"SHIFT"/"TAB"/"ESC"/"ENTER"/"UP"/"DOWN"/"LEFT"/"RIGHT"/"F1".."F12")。
// Lua の KEY_* と同じ割り当て(ScriptEngine.cpp)。
inline int ParseMcpVk(const nlohmann::json& params)
{
    auto it = params.find("key");
    if (it == params.end()) throw McpError(McpErr::InvalidParam, "missing 'key'");
    if (it->is_number_integer())
    {
        int vk = it->get<int>();
        if (vk < 0 || vk > 255) throw McpError(McpErr::InvalidParam, "key (VK) must be 0..255");
        return vk;
    }
    if (!it->is_string()) throw McpError(McpErr::InvalidParam, "key must be a VK int or a key name string");
    std::string s = it->get<std::string>();
    for (auto& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    if (s.size() == 1)
    {
        char c = s[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return static_cast<int>(c);
    }
    if (s == "SPACE")  return VK_SPACE;
    if (s == "SHIFT")  return VK_SHIFT;
    if (s == "CTRL" || s == "CONTROL") return VK_CONTROL;
    if (s == "ALT")    return VK_MENU;
    if (s == "TAB")    return VK_TAB;
    if (s == "ESC" || s == "ESCAPE")   return VK_ESCAPE;
    if (s == "ENTER" || s == "RETURN") return VK_RETURN;
    if (s == "UP")     return VK_UP;
    if (s == "DOWN")   return VK_DOWN;
    if (s == "LEFT")   return VK_LEFT;
    if (s == "RIGHT")  return VK_RIGHT;
    if (s[0] == 'F' && (s.size() == 2 || s.size() == 3))   // F1..F12
    {
        int n = (s.size() == 2) ? (s[1] - '0') : (s[1] - '0') * 10 + (s[2] - '0');
        if (n >= 1 && n <= 12) return VK_F1 + (n - 1);
    }
    throw McpError(McpErr::InvalidParam, "unknown key name: " + s);
}

// ════════════════════════════════════════════════════════════════
//  MCP: 引数パースの共通部品（範囲外・列挙ミスを「次に何をすればいいか」付きで返す）
// ════════════════════════════════════════════════════════════════

// 小数を短く文字列化する（エラーメッセージ用。std::to_string は "12.000000" になって読みにくい）。
inline std::string McpNum(f32 v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

// 数値パラメータ（省略時 defVal）。範囲外は「有効範囲」を添えて弾く。
inline f32 McpFloatParam(const nlohmann::json& p, const char* key, f32 defVal, f32 lo, f32 hi)
{
    auto it = p.find(key);
    if (it == p.end() || it->is_null()) return defVal;
    if (!it->is_number())
        throw McpError(McpErr::InvalidParam, std::string(key) + " must be a number");
    const f32 v = it->get<f32>();
    if (!(v >= lo && v <= hi))
        throw McpError(McpErr::InvalidParam,
                       std::string(key) + " が範囲外: " + McpNum(v),
                       std::string("有効範囲は ") + McpNum(lo) + " 〜 " + McpNum(hi) + " や");
    return v;
}

inline i32 McpIntParam(const nlohmann::json& p, const char* key, i32 defVal, i32 lo, i32 hi)
{
    auto it = p.find(key);
    if (it == p.end() || it->is_null()) return defVal;
    if (!it->is_number())
        throw McpError(McpErr::InvalidParam, std::string(key) + " must be an integer");
    const i32 v = it->get<i32>();
    if (v < lo || v > hi)
        throw McpError(McpErr::InvalidParam,
                       std::string(key) + " が範囲外: " + std::to_string(v),
                       std::string("有効範囲は ") + std::to_string(lo) + " 〜 " + std::to_string(hi) + " や");
    return v;
}

// 列挙パラメータ。未知の値は「有効値の一覧」を error_values に載せて返す＝AI が推測しない。
inline int McpEnumParam(const nlohmann::json& p, const char* key,
                 const std::vector<std::string>& values, int defIndex, const char* hint = "")
{
    auto it = p.find(key);
    if (it == p.end() || it->is_null()) return defIndex;
    if (!it->is_string())
        throw McpError(McpErr::InvalidParam, std::string(key) + " must be a string", hint, values);
    std::string s = it->get<std::string>();
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    for (size_t i = 0; i < values.size(); ++i)
        if (values[i] == s) return static_cast<int>(i);
    throw McpError(McpErr::InvalidParam, "unknown " + std::string(key) + ": " + s,
                   hint[0] ? hint : "有効値のどれかを指定してくれ", values);
}

// [x,y,z] を読む（必須）。
inline DirectX::XMFLOAT3 McpVec3Required(const nlohmann::json& p, const char* key)
{
    auto it = p.find(key);
    if (it == p.end() || !it->is_array() || it->size() != 3)
        throw McpError(McpErr::InvalidParam, std::string("missing/invalid '") + key + "'",
                       std::string(key) + " は [x,y,z] の数値3要素で渡す");
    return { (*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>() };
}

// [x,y,z] を読む（任意）。無ければ false。
inline bool McpTryVec3(const nlohmann::json& p, const char* key, DirectX::XMFLOAT3& out)
{
    auto it = p.find(key);
    if (it == p.end() || it->is_null()) return false;
    if (!it->is_array() || it->size() != 3)
        throw McpError(McpErr::InvalidParam, std::string("invalid '") + key + "'",
                       std::string(key) + " は [x,y,z] の数値3要素で渡す");
    out = { (*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>() };
    return true;
}

// ════════════════════════════════════════════════════════════════
//  MCP: 精密ピッキング（ScenePick）の結果整形
// ════════════════════════════════════════════════════════════════

// ヒット 1 件 → JSON。エディタのクリック選択とまったく同じ構造体を出すので、
// 「MCP が見たもの」と「エディタで選ばれるもの」がズレない。
inline nlohmann::json McpPickHitJson(const entt::registry& reg, const ScenePickHit& h)
{
    nlohmann::json j{
        {"entityId",     static_cast<u32>(h.entity)},
        {"submeshIndex", h.submeshIndex},
        {"distance",     h.distance},
        {"worldPos",     {h.worldPos.x, h.worldPos.y, h.worldPos.z}},
        {"worldNormal",  {h.worldNormal.x, h.worldNormal.y, h.worldNormal.z}},
        {"isIcon",       h.isIcon},
    };
    if (reg.valid(h.entity) && reg.all_of<NameTag>(h.entity))
        j["name"] = reg.get<NameTag>(h.entity).name;
    return j;
}

// ヒット列 → {hits, count, totalHits, truncated}。all=false なら最前面 1 件だけ返す。
inline nlohmann::json McpPickHitsJson(const entt::registry& reg,
                               const std::vector<ScenePickHit>& hits,
                               bool all, int maxHits)
{
    nlohmann::json arr = nlohmann::json::array();
    const size_t limit = all ? static_cast<size_t>(maxHits) : (hits.empty() ? 0u : 1u);
    for (size_t i = 0; i < hits.size() && arr.size() < limit; ++i)
        arr.push_back(McpPickHitJson(reg, hits[i]));
    return nlohmann::json{
        {"hits", arr},
        {"count", arr.size()},
        {"totalHits", hits.size()},
        {"truncated", all && hits.size() > arr.size()},
    };
}

// ════════════════════════════════════════════════════════════════
//  MCP: 地形 / スカルプトの対象解決
// ════════════════════════════════════════════════════════════════

// T(Terrain / SculptMesh) を持つエンティティを解決する。
// entity/name の指定があればそれを使い、無ければシーンに 1 個だけなら自動採用する
//（2 個以上あるのに指定が無いのは曖昧なので名指しを促す＝黙って違うものを彫らない）。
template <typename T>
entt::entity ResolveMcpComponentEntity(Scene& scene, const nlohmann::json& params,
                                       const char* jsonKey, const char* createHint)
{
    auto& reg = scene.GetRegistry();
    if (params.contains("entity") || params.contains("name"))
    {
        const auto e = ResolveMcpEntity(scene, params);
        if (!reg.all_of<T>(e))
            throw McpError(McpErr::InvalidParam,
                           std::string("entity has no ") + jsonKey + " component", createHint);
        return e;
    }
    entt::entity found = entt::null;
    int n = 0;
    for (auto e : reg.view<T>())
    {
        if (n == 0) found = e;
        ++n;
    }
    if (n == 1) return found;
    if (n == 0)
        throw McpError(McpErr::NotFound,
                       std::string("no entity with ") + jsonKey + " in this scene", createHint);
    throw McpError(McpErr::InvalidParam,
                   std::string("multiple ") + jsonKey + " entities (" + std::to_string(n) + ")",
                   std::string("entity(id) か name でどれを触るか指定してくれ。一覧は ")
                       + "dx12_list_entities(component_type:\"" + jsonKey + "\")");
}

// 地形の原点（ワールド）。ハイトフィールドは XZ グリッド前提なので回転/スケールは無視する
//（TerrainPanel::TerrainOrigin と同じ規約）。
inline DirectX::XMFLOAT3 McpTerrainOrigin(const entt::registry& reg, entt::entity e)
{
    DirectX::XMFLOAT3 out{0.0f, 0.0f, 0.0f};
    if (!reg.all_of<Transform>(e)) return out;
    DirectX::XMStoreFloat3(&out, ComputeWorldMatrix(reg, e).r[3]);
    return out;
}

// エンジン自身(自 exe)を子プロセスとして起動し、終了(または timeoutMs)まで待つ。
// dx12_validate_scene の --validate ヘッドレス実行に使う。--validate は main.cpp で
// GPU/ウィンドウ初期化より前に return するため、エディタ実行中でも安全に並行起動できる。
// 戻り値: 終了コード(起動失敗やタイムアウトは 1 = FAIL 扱い)。
inline int RunEngineSubprocessAndWait(const std::string& exePath, const std::string& args,
                               const std::string& workDir, DWORD timeoutMs)
{
    std::string cmd = "\"" + exePath + "\" " + args;
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr,
                             workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    if (!ok) return 1;
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (wait == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// 遅延応答の送信ヘルパ。client==0(=MCP 由来でない) は何もしない。
inline void SendMcp(McpBridge* bridge, const McpDeferred& d, nlohmann::json resp)
{
    if (!bridge || d.client == 0) return;
    resp["id"] = d.requestId;
    // 不正 UTF-8(CP932 のモデル名由来 NameTag 等)で dump が投げないよう replace。
    bridge->SendToClient(d.client,
        resp.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}
inline void CompleteMcp(McpBridge* bridge, const McpDeferred& d, nlohmann::json result)
{
    SendMcp(bridge, d, nlohmann::json{{"ok", true}, {"result", std::move(result)}});
}
inline void FailMcp(McpBridge* bridge, const McpDeferred& d, int code, const std::string& msg)
{
    SendMcp(bridge, d, nlohmann::json{{"ok", false}, {"error", msg}, {"error_code", code}});
}

// dx12_describe_components 用のコンポーネントスキーマ表。
// AI がフィールド名/型/既定値を推測せず set_component を正しく呼べるようにする。
// jsonKey は get_entity が返し set_component/remove_component が受けるキー。
// settable=set_component 可 / removable=remove_component 可。
// Lua コンポーネントスクリプトから entity プロパティとして直接読めるか。
// Entity usertype が公開しているデータプロパティは transform だけ(ScriptEngine.cpp の new_usertype<Entity>)。
// boxCollider/rigidBody など他は entity.<key> では nil になる(physics:getVelocity 等の別経路のみ)。
inline bool LuaReadableComponent(const std::string& jsonKey)
{
    return jsonKey == "transform";
}

nlohmann::json McpComponentSchema();

// dx12_describe_lua_api 用。Lua コンポーネントスクリプトから使えるバインディングの静的辞書。
// 実体は ScriptEngine.cpp の new_usertype/グローバル(ハンドコード)。ここは「何が呼べるか」を
// バインディングオブジェクトごとに列挙するだけ(ランタイムリフレクションはしない)。MCP 上で見える
// コンポーネントと Lua から読める API のズレ(例: entity.boxCollider は nil)を AI に明示するのが目的。
nlohmann::json McpLuaApi();

// entity が持つコンポーネントの jsonKey 一覧(list_entities verbose / get_entity 概況)。
inline nlohmann::json McpComponentTypesOf(const entt::registry& reg, entt::entity e)
{
    nlohmann::json a = nlohmann::json::array();
    if (reg.all_of<Transform>(e))           a.push_back("transform");
    if (reg.all_of<MeshRenderer>(e))        a.push_back("meshRenderer");
    if (reg.all_of<PointLight>(e))          a.push_back("pointLight");
    if (reg.all_of<DirectionalLight>(e))    a.push_back("directionalLight");
    if (reg.all_of<SpotLight>(e))           a.push_back("spotLight");
    if (reg.all_of<CameraComponent>(e))     a.push_back("camera");
    if (reg.all_of<RigidBody>(e))           a.push_back("rigidBody");
    if (reg.all_of<BoxCollider>(e))         a.push_back("boxCollider");
    if (reg.all_of<SphereCollider>(e))      a.push_back("sphereCollider");
    if (reg.all_of<CapsuleCollider>(e))     a.push_back("capsuleCollider");
    if (reg.all_of<CharacterController>(e))  a.push_back("characterController");
    if (reg.all_of<ConvexHullCollider>(e))  a.push_back("convexHullCollider");
    if (reg.all_of<Sprite2D>(e))            a.push_back("sprite2d");
    if (reg.all_of<Tag>(e))                 a.push_back("tags");
    if (reg.all_of<DataComponent>(e))       a.push_back("data");
    if (reg.all_of<AudioSource>(e))         a.push_back("audioSource");
    if (reg.all_of<ParticleEmitter>(e))     a.push_back("particleEmitter");
    if (reg.all_of<Trigger>(e))             a.push_back("trigger");
    if (reg.all_of<Gimmick>(e))             a.push_back("gimmick");
    if (reg.all_of<LuaScript>(e))           a.push_back("luaScript");
    if (reg.all_of<TrailRenderer>(e))       a.push_back("trailRenderer");
    if (reg.all_of<DecalComponent>(e))      a.push_back("decal");
    if (reg.all_of<NetworkIdentity>(e))     a.push_back("networkIdentity");
    if (reg.all_of<NetworkTransform>(e))    a.push_back("networkTransform");
    if (reg.all_of<Terrain>(e))             a.push_back("terrain");
    if (reg.all_of<SculptMesh>(e))          a.push_back("sculptMesh");
    if (reg.all_of<SkeletalAnimation>(e))   a.push_back("skeletalAnimation");
    if (reg.all_of<UIAnimPlayer>(e))        a.push_back("uiAnimPlayer");
    if (reg.all_of<SpriteAnimator>(e))      a.push_back("spriteAnimator");
    if (reg.all_of<AnimatorController>(e))  a.push_back("animatorController");
    if (reg.all_of<FootIK>(e))              a.push_back("footIK");
    if (reg.all_of<PrefabLink>(e))          a.push_back("prefabLink");
    return a;
}



// ---------------------------------------------------------------------------
// MCP: method 名 → ハンドラのディスパッチ表（#30。N37 / N43 の根治）
// ---------------------------------------------------------------------------
// ★以前は HandleMcpCommand の中に `else if (method == "...")` が 118 本並んでいて、
//   MSVC の「ブロックの入れ子のレベルが深すぎます (C1061)」上限に張り付いていた。
//   1 本足すだけでコンパイルが落ち、逃げ道として get/set を 1 ブロックへ束ねたり
//   HandleMcpRenderCommand() へ早期 return させたりしていた（N37 / N43）。
//   **表引きにしたので method を何本足しても入れ子は 1 段も深くならない。**
//   HandleMcpRenderCommand() は役目を終えたので削除した。
//
// 新しい method の足し方（これだけ）:
//   1. 下の Register***McpMethods() のどれかへ
//        McpDefine("名前", "キー:型,...", DX12E_MCP_HANDLER { ... });
//      を 1 本足す（置く場所はテーマで選ぶ。順序に意味は無い＝表引きなので）。
//   2. 第 2 引数のキー表は `dx12_describe_mcp_params` がそのまま返す。
//      **本文で読むキーと必ず一致させること**（tests/mcp_param_spec_test が機械的に見張る）。
//   3. docs/MCP.md と tools/mcp-server/index.ts の zod スキーマにも同じキーを足す
//      （足し忘れると zod が黙って引数を捨てる。N21）。
//
// ★ハンドラの引数名は旧チェーンのローカル変数と同じ（params / resp / method /
//   deferred / isDeferred / busyPlaying）。おかげで 118 本の本文を 1 行も書き換えずに移設できた。
//   使わない引数があっても警告が出ないよう [[maybe_unused]] を付けてある。
#define DX12E_MCP_HANDLER                                                    \
    [this]([[maybe_unused]] const nlohmann::json& params,                    \
           [[maybe_unused]] nlohmann::json&       resp,                      \
           [[maybe_unused]] const std::string&    method,                    \
           [[maybe_unused]] McpDeferred&          deferred,                  \
           [[maybe_unused]] bool&                 isDeferred,                \
           [[maybe_unused]] bool                  busyPlaying) -> void

// ポスト / SSAO のキー表は本文に書かず、**唯一の名前表**である X マクロから組み立てる
// （計画03 Phase 3。`PostProcessSettings.h` のコメントが予告していたもの）。
// フィールドを足したら X マクロ 1 箇所を直すだけで MCP のキー表も追従する。
inline const char* McpPostParamSpec()
{
    static const std::string spec = []
    {
        std::string t;
#define DX12E_MCP_PP_B(f) t += #f ":bool,";
#define DX12E_MCP_PP_F(f) t += #f ":number,";
#define DX12E_MCP_PP_I(f) t += #f ":int,";
#define DX12E_MCP_PP_V(f) t += #f ":vec3,";
#define DX12E_MCP_PP_S(f) t += #f ":string,";
        DX12E_POST_FIELDS(DX12E_MCP_PP_B, DX12E_MCP_PP_F, DX12E_MCP_PP_I,
                          DX12E_MCP_PP_V, DX12E_MCP_PP_S)
#undef DX12E_MCP_PP_B
#undef DX12E_MCP_PP_F
#undef DX12E_MCP_PP_I
#undef DX12E_MCP_PP_V
#undef DX12E_MCP_PP_S
        if (!t.empty()) t.pop_back();
        return t;
    }();
    return spec.c_str();
}

inline const char* McpSsaoParamSpec()
{
    static const std::string spec = []
    {
        std::string t;
#define DX12E_MCP_SS_B(f) t += #f ":bool,";
#define DX12E_MCP_SS_F(f) t += #f ":number,";
#define DX12E_MCP_SS_I(f) t += #f ":int,";
        DX12E_SSAO_FIELDS(DX12E_MCP_SS_B, DX12E_MCP_SS_F, DX12E_MCP_SS_I)
#undef DX12E_MCP_SS_B
#undef DX12E_MCP_SS_F
#undef DX12E_MCP_SS_I
        if (!t.empty()) t.pop_back();
        return t;
    }();
    return spec.c_str();
}

// ---- PNG 書き出し / ウィンドウキャプチャ（定義は ApplicationInternal.cpp）----
bool WriteBgraPng(const std::wstring& path, const uint8_t* bgra,
                  uint32_t w, uint32_t h, std::string& err);
std::string CaptureWindowScreenshot(HWND hwnd, std::string& err);

} // namespace appdetail
} // namespace dx12e
