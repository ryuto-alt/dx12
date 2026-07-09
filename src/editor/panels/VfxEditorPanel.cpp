#include "editor/panels/VfxEditorPanel.h"
#include "editor/EditorContext.h"
#include "editor/UndoSystem.h"
#include "ecs/Components.h"
#include "graphics/GraphicsDevice.h"
#include "graphics/CommandList.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable: 4189 4456 4458 4267 4996)
#include <nlohmann/json.hpp>
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

using namespace DirectX;
namespace fs = std::filesystem;

namespace dx12e
{
namespace
{
nlohmann::json SerializeFloat3(const XMFLOAT3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }

XMFLOAT3 DeserializeFloat3(const nlohmann::json& j, XMFLOAT3 def = {0.0f, 0.0f, 0.0f})
{
    if (!j.is_array() || j.size() < 3) return def;
    return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
}

const char* kKindNames[] = { "Glow", "Fire", "Smoke", "Spark", "Magic", "Electric", "Ring", "Star" };
const char* kLuaKindNames[] = { "glow", "fire", "smoke", "spark", "magic", "electric", "ring", "star" };
} // namespace

void VfxEditorPanel::Initialize(GraphicsDevice& device, DescriptorHeap* srvHeap, ResourceManager* resourceManager,
                                const std::wstring& shaderDir)
{
    m_device  = &device;
    m_srvHeap = srvHeap;

    m_previewRtvHeap.Initialize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
    constexpr float clearColor[4] = {0.03f, 0.03f, 0.045f, 1.0f};
    m_previewRT.Initialize(device, &m_previewRtvHeap, srvHeap, kPreviewSize, kPreviewSize,
                           DXGI_FORMAT_R8G8B8A8_UNORM, clearColor);

    // LDR ターゲット向け PSO を自前で構築する独立インスタンス（メインの m_particleSystem とは別物）。
    // srvHeap/resourceManager を渡すことでプレビューでもテクスチャ貼り付けが確認できる。
    m_previewParticles.Initialize(device, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, shaderDir,
                                  srvHeap, resourceManager);

    m_gfxInitialized = true;
    NewAsset();
}

void VfxEditorPanel::RecreatePipelines(GraphicsDevice& device)
{
    m_previewParticles.RecreatePipelines(device);
}

void VfxEditorPanel::RenderPreview3D(EditorContext& ctx, CommandList& cmd, f32 dt)
{
    if (!ctx.showVfxEditor || !m_gfxInitialized) return;

    m_previewTime += dt;
    m_previewParticles.SetTime(m_previewTime);

    // プレビューは rate<=0 でも見た目を確認できるよう最低放出量を保証する。
    const f32 previewRate = (m_current.rate > 0.0f) ? m_current.rate : 20.0f;
    m_emitAccum += previewRate * dt;
    int n = static_cast<int>(m_emitAccum);
    if (n > 0)
    {
        m_emitAccum -= static_cast<f32>(n);
        if (n > 64) n = 64;

        ParticleSystem::EmitParams p;
        p.pos = {0.0f, 0.0f, 0.0f};
        p.count = n;
        p.dir = m_current.dir;               p.spread   = m_current.spread;
        p.speed = m_current.speed;           p.speedVar = m_current.speedVar;
        p.size = m_current.size;             p.sizeEnd  = m_current.sizeEnd;
        p.sizeMid = m_current.sizeMid;
        p.life = m_current.life;             p.lifeVar  = m_current.lifeVar;
        p.color = m_current.color;           p.colorEnd = m_current.colorEnd;
        p.colorMid = m_current.colorMid;     p.hasColorMid = m_current.hasColorMid;
        p.hasColorEnd = true;
        p.intensity = m_current.intensity;
        p.gravity = m_current.gravity;       p.drag = m_current.drag; p.up = m_current.up;
        p.stretch = m_current.stretch;       p.kind = m_current.kind; p.blend = m_current.blend;
        p.turbStrength = m_current.turbStrength; p.turbFreq = m_current.turbFreq;
        p.flicker = m_current.flicker;       p.flickerFreq = m_current.flickerFreq;
        p.texturePath = m_current.texturePath;
        m_previewParticles.Emit(p);
    }
    m_previewParticles.Update(dt);

    // オービットカメラ（yaw/pitch/dist の球面座標 → LookAt）
    const XMVECTOR target = XMLoadFloat3(&m_camTarget);
    const f32 cp = std::cos(m_camPitch), sp = std::sin(m_camPitch);
    const f32 cy = std::cos(m_camYaw),   sy = std::sin(m_camYaw);
    const XMVECTOR offset = XMVectorScale(XMVectorSet(cp * cy, sp, cp * sy, 0.0f), m_camDist);
    const XMVECTOR eye = XMVectorAdd(target, offset);
    const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(50.0f), 1.0f, 0.05f, 100.0f);
    const XMMATRIX viewProj = view * proj;

    const XMMATRIX invView = XMMatrixInverse(nullptr, view);
    XMFLOAT3 camRight, camUp, camPos;
    XMStoreFloat3(&camRight, invView.r[0]);
    XMStoreFloat3(&camUp,    invView.r[1]);
    XMStoreFloat3(&camPos,   invView.r[3]);

    m_previewRT.Transition(cmd, D3D12_RESOURCE_STATE_RENDER_TARGET);
    constexpr float clearColor[4] = {0.03f, 0.03f, 0.045f, 1.0f};
    cmd.ClearRenderTarget(m_previewRT.GetRtv(), clearColor);

    ID3D12GraphicsCommandList* native = cmd.GetNative();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_previewRT.GetRtv();
    native->OMSetRenderTargets(1, &rtv, FALSE, nullptr);   // 深度なし（パーティクルのみのプレビュー）
    cmd.SetViewportAndScissor(kPreviewSize, kPreviewSize);

    m_previewParticles.DisableSceneDepth();   // ソフトパーティクル無効（シーン深度が存在しないため）
    m_previewParticles.Render(native, viewProj, camRight, camUp, camPos);

    m_previewRT.Transition(cmd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void VfxEditorPanel::RefreshAssetList(const std::string& assetsDir)
{
    m_assetNames.clear();
    const std::string dir = assetsDir + "vfx/";
    std::error_code ec;
    if (fs::exists(dir, ec))
    {
        for (auto& entry : fs::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;
            m_assetNames.push_back(entry.path().stem().string());
        }
        std::sort(m_assetNames.begin(), m_assetNames.end());
    }
    m_assetListLoaded = true;
}

void VfxEditorPanel::NewAsset()
{
    m_current = VfxAsset{};
    m_currentPath.clear();
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_current.name.c_str());
    if (m_gfxInitialized) m_previewParticles.Clear();
}

bool VfxEditorPanel::LoadAsset(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) return false;
    nlohmann::json j;
    try { f >> j; } catch (...) { return false; }

    VfxAsset a;
    a.name        = j.value("name", fs::path(path).stem().string());
    a.kind        = j.value("kind", 1);
    a.blend       = j.value("blend", 0);
    a.rate        = j.value("rate", 30.0f);
    a.playOnStart = j.value("playOnStart", true);
    a.looping     = j.value("looping", true);
    a.duration    = j.value("duration", 1.0f);
    a.burstCount  = j.value("burstCount", 24);
    if (j.contains("dir")) a.dir = DeserializeFloat3(j["dir"], {0.0f, 1.0f, 0.0f});
    a.spread   = j.value("spread", 0.4f);
    a.speed    = j.value("speed", 3.0f);
    a.speedVar = j.value("speedVar", 0.4f);
    a.size     = j.value("size", 0.3f);
    a.sizeMid  = j.value("sizeMid", -1.0f);
    a.sizeEnd  = j.value("sizeEnd", 0.0f);
    a.life     = j.value("life", 0.8f);
    a.lifeVar  = j.value("lifeVar", 0.3f);
    if (j.contains("color"))    a.color    = DeserializeFloat3(j["color"],    {1.0f, 0.6f, 0.2f});
    if (j.contains("colorMid")) a.colorMid = DeserializeFloat3(j["colorMid"], {1.0f, 0.6f, 0.2f});
    if (j.contains("colorEnd")) a.colorEnd = DeserializeFloat3(j["colorEnd"], {1.0f, 0.12f, 0.05f});
    a.hasColorMid = j.value("hasColorMid", false);
    a.intensity   = j.value("intensity", 3.0f);
    a.gravity     = j.value("gravity", 0.0f);
    a.drag        = j.value("drag", 1.0f);
    a.up          = j.value("up", 0.0f);
    a.stretch     = j.value("stretch", 0.0f);
    a.turbStrength = j.value("turbStrength", 0.0f);
    a.turbFreq     = j.value("turbFreq", 1.0f);
    a.flicker      = j.value("flicker", 0.0f);
    a.flickerFreq  = j.value("flickerFreq", 18.0f);
    a.distort   = j.value("distort", 0.0f);
    a.light     = j.value("light", false);
    a.lightRange = j.value("lightRange", 3.0f);
    a.texturePath = j.value("texturePath", std::string());

    m_current = a;
    m_currentPath = path;
    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", a.name.c_str());
    if (m_gfxInitialized) m_previewParticles.Clear();
    return true;
}

bool VfxEditorPanel::SaveAsset(const std::string& path)
{
    nlohmann::json j;
    j["name"] = m_current.name;
    j["kind"] = m_current.kind;   j["blend"] = m_current.blend;
    j["rate"] = m_current.rate;   j["playOnStart"] = m_current.playOnStart;
    j["looping"] = m_current.looping; j["duration"] = m_current.duration;
    j["burstCount"] = m_current.burstCount;
    j["dir"] = SerializeFloat3(m_current.dir); j["spread"] = m_current.spread;
    j["speed"] = m_current.speed; j["speedVar"] = m_current.speedVar;
    j["size"] = m_current.size; j["sizeMid"] = m_current.sizeMid; j["sizeEnd"] = m_current.sizeEnd;
    j["life"] = m_current.life; j["lifeVar"] = m_current.lifeVar;
    j["color"]    = SerializeFloat3(m_current.color);
    j["colorMid"] = SerializeFloat3(m_current.colorMid);
    j["colorEnd"] = SerializeFloat3(m_current.colorEnd);
    j["hasColorMid"] = m_current.hasColorMid;
    j["intensity"] = m_current.intensity;
    j["gravity"] = m_current.gravity; j["drag"] = m_current.drag;
    j["up"] = m_current.up; j["stretch"] = m_current.stretch;
    j["turbStrength"] = m_current.turbStrength; j["turbFreq"] = m_current.turbFreq;
    j["flicker"] = m_current.flicker; j["flickerFreq"] = m_current.flickerFreq;
    j["distort"] = m_current.distort; j["light"] = m_current.light; j["lightRange"] = m_current.lightRange;
    j["texturePath"] = m_current.texturePath;

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << j.dump(2);
    return true;
}

void VfxEditorPanel::ApplyToSelected(entt::registry& reg, EditorContext& ctx)
{
    entt::entity e = ctx.selectedEntity;
    if (e == entt::null || !reg.valid(e) || !reg.all_of<ParticleEmitter>(e)) return;

    const ParticleEmitter before = reg.get<ParticleEmitter>(e);
    ParticleEmitter after = before;
    after.kind = m_current.kind;   after.blend = m_current.blend;
    after.rate = m_current.rate;   after.playOnStart = m_current.playOnStart;
    after.looping = m_current.looping; after.duration = m_current.duration;
    after.dir = m_current.dir;     after.spread = m_current.spread;
    after.speed = m_current.speed; after.speedVar = m_current.speedVar;
    after.size = m_current.size;   after.sizeEnd = m_current.sizeEnd; after.sizeMid = m_current.sizeMid;
    after.life = m_current.life;   after.lifeVar = m_current.lifeVar;
    after.color = m_current.color; after.colorMid = m_current.colorMid; after.colorEnd = m_current.colorEnd;
    after.hasColorMid = m_current.hasColorMid;
    after.intensity = m_current.intensity;
    after.gravity = m_current.gravity; after.drag = m_current.drag; after.up = m_current.up;
    after.stretch = m_current.stretch;
    after.turbStrength = m_current.turbStrength; after.turbFreq = m_current.turbFreq;
    after.distort = m_current.distort;
    after.light = m_current.light; after.lightRange = m_current.lightRange;
    after.flicker = m_current.flicker; after.flickerFreq = m_current.flickerFreq;
    after.texturePath = m_current.texturePath;
    // gpu はこのエディタの対象外（CPUパーティクルのまま据え置く）

    reg.get<ParticleEmitter>(e) = after;
    ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<ParticleEmitter>>(
        &reg, e, before, after, "VFX アセットを適用"));
    m_statusMsg = "選択エンティティへ適用しました";
    m_statusFlash = 2.0f;
}

void VfxEditorPanel::SpawnEntity(entt::registry& reg, EditorContext& ctx)
{
    ParticleEmitter pe{};
    pe.kind = m_current.kind;   pe.blend = m_current.blend;
    pe.rate = m_current.rate;   pe.playOnStart = m_current.playOnStart;
    pe.looping = m_current.looping; pe.duration = m_current.duration;
    pe.dir = m_current.dir;     pe.spread = m_current.spread;
    pe.speed = m_current.speed; pe.speedVar = m_current.speedVar;
    pe.size = m_current.size;   pe.sizeEnd = m_current.sizeEnd; pe.sizeMid = m_current.sizeMid;
    pe.life = m_current.life;   pe.lifeVar = m_current.lifeVar;
    pe.color = m_current.color; pe.colorMid = m_current.colorMid; pe.colorEnd = m_current.colorEnd;
    pe.hasColorMid = m_current.hasColorMid;
    pe.intensity = m_current.intensity;
    pe.gravity = m_current.gravity; pe.drag = m_current.drag; pe.up = m_current.up;
    pe.stretch = m_current.stretch;
    pe.turbStrength = m_current.turbStrength; pe.turbFreq = m_current.turbFreq;
    pe.distort = m_current.distort;
    pe.light = m_current.light; pe.lightRange = m_current.lightRange;
    pe.flicker = m_current.flicker; pe.flickerFreq = m_current.flickerFreq;
    pe.texturePath = m_current.texturePath;

    XMFLOAT3 spawnPos{0.0f, 0.0f, 0.0f};
    if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity) &&
        reg.all_of<Transform>(ctx.selectedEntity))
        spawnPos = reg.get<Transform>(ctx.selectedEntity).position;

    entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{ m_current.name.empty() ? std::string("ParticleEmitter") : m_current.name });
    reg.emplace<Transform>(e, Transform{ spawnPos, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f} });
    reg.emplace<ParticleEmitter>(e, pe);
    ctx.Select(e);
    m_statusMsg = "新規エンティティとして配置しました";
    m_statusFlash = 2.0f;
}

std::string VfxEditorPanel::BuildLuaSnippet() const
{
    const char* kindName  = (m_current.kind >= 0 && m_current.kind < 8) ? kLuaKindNames[m_current.kind] : "glow";
    const char* blendName = (m_current.blend == 1) ? "alpha" : "additive";

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3);
    ss << "-- " << m_current.name << "（パーティクルエディタで作成）\n";
    ss << "fx:burst{\n";
    ss << "  x=0, y=0, z=0, count=" << m_current.burstCount << ",\n";
    ss << "  dx=" << m_current.dir.x << ", dy=" << m_current.dir.y << ", dz=" << m_current.dir.z
       << ", spread=" << m_current.spread << ",\n";
    ss << "  speed=" << m_current.speed << ", speedVar=" << m_current.speedVar << ",\n";
    ss << "  size=" << m_current.size << ", sizeEnd=" << m_current.sizeEnd;
    if (m_current.sizeMid >= 0.0f) ss << ", sizeMid=" << m_current.sizeMid;
    ss << ",\n";
    ss << "  life=" << m_current.life << ", lifeVar=" << m_current.lifeVar << ",\n";
    ss << "  r=" << m_current.color.x << ", g=" << m_current.color.y << ", b=" << m_current.color.z << ",\n";
    ss << "  rEnd=" << m_current.colorEnd.x << ", gEnd=" << m_current.colorEnd.y
       << ", bEnd=" << m_current.colorEnd.z << ",\n";
    if (m_current.hasColorMid)
        ss << "  rMid=" << m_current.colorMid.x << ", gMid=" << m_current.colorMid.y
           << ", bMid=" << m_current.colorMid.z << ",\n";
    ss << "  kind=\"" << kindName << "\", blend=\"" << blendName << "\",\n";
    ss << "  intensity=" << m_current.intensity << ", gravity=" << m_current.gravity
       << ", drag=" << m_current.drag << ", up=" << m_current.up
       << ", stretch=" << m_current.stretch << ",\n";
    if (m_current.turbStrength > 0.0f)
        ss << "  turbStrength=" << m_current.turbStrength << ", turbFreq=" << m_current.turbFreq << ",\n";
    if (m_current.flicker > 0.0f)
        ss << "  flicker=" << m_current.flicker << ", flickerFreq=" << m_current.flickerFreq << ",\n";
    if (m_current.distort > 0.0f)
        ss << "  distort=" << m_current.distort << ",\n";
    if (m_current.light)
        ss << "  light=true, lightRange=" << m_current.lightRange << ",\n";
    ss << "}\n";
    return ss.str();
}

void VfxEditorPanel::DrawColorGradient()
{
    ImGui::TextUnformatted("色の変化 Color Over Life");
    ImGui::Checkbox("中間色を使う HasColorMid", &m_current.hasColorMid);

    const float barW = ImGui::GetContentRegionAvail().x;
    const float barH = 26.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto toU32 = [](const XMFLOAT3& c) {
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, 1.0f));
    };

    if (m_current.hasColorMid)
    {
        const float midX = p0.x + barW * 0.5f;
        dl->AddRectFilledMultiColor(p0, ImVec2(midX, p0.y + barH),
            toU32(m_current.color), toU32(m_current.colorMid),
            toU32(m_current.colorMid), toU32(m_current.color));
        dl->AddRectFilledMultiColor(ImVec2(midX, p0.y), ImVec2(p0.x + barW, p0.y + barH),
            toU32(m_current.colorMid), toU32(m_current.colorEnd),
            toU32(m_current.colorEnd), toU32(m_current.colorMid));
    }
    else
    {
        dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + barW, p0.y + barH),
            toU32(m_current.color), toU32(m_current.colorEnd),
            toU32(m_current.colorEnd), toU32(m_current.color));
    }
    dl->AddRect(p0, ImVec2(p0.x + barW, p0.y + barH), IM_COL32(90, 90, 100, 255));
    ImGui::Dummy(ImVec2(barW, barH + 4.0f));

    ImGui::SetNextItemWidth(120);
    ImGui::ColorEdit3("開始 Start##col", &m_current.color.x);
    if (m_current.hasColorMid)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::ColorEdit3("中間 Mid##col", &m_current.colorMid.x);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::ColorEdit3("終了 End##col", &m_current.colorEnd.x);
}

void VfxEditorPanel::DrawSizeCurve()
{
    ImGui::TextUnformatted("サイズの変化 Size Over Life");

    bool useMid = (m_current.sizeMid >= 0.0f);
    if (ImGui::Checkbox("中間サイズを使う HasSizeMid", &useMid))
        m_current.sizeMid = useMid ? (m_current.size + m_current.sizeEnd) * 0.5f : -1.0f;

    const float graphW = ImGui::GetContentRegionAvail().x;
    const float graphH = 84.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1 = ImVec2(p0.x + graphW, p0.y + graphH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(22, 22, 28, 255));
    dl->AddRect(p0, p1, IM_COL32(90, 90, 100, 255));

    const float maxV = (std::max)({m_current.size, m_current.sizeEnd,
                                    useMid ? m_current.sizeMid : 0.0f, 0.05f}) * 1.35f;
    auto toPt = [&](float t, float v) {
        return ImVec2(p0.x + t * graphW, p1.y - (v / maxV) * graphH);
    };

    const ImVec2 ptStart = toPt(0.0f, m_current.size);
    const ImVec2 ptEnd   = toPt(1.0f, m_current.sizeEnd);
    if (useMid)
    {
        const ImVec2 ptMid = toPt(0.5f, m_current.sizeMid);
        dl->AddLine(ptStart, ptMid, IM_COL32(130, 200, 255, 255), 2.0f);
        dl->AddLine(ptMid, ptEnd,   IM_COL32(130, 200, 255, 255), 2.0f);
    }
    else
    {
        dl->AddLine(ptStart, ptEnd, IM_COL32(130, 200, 255, 255), 2.0f);
    }

    ImGui::PushID("SizeCurve");
    auto handle = [&](const char* id, ImVec2 pos, float& value) {
        ImGui::SetCursorScreenPos(ImVec2(pos.x - 6.0f, pos.y - 6.0f));
        ImGui::InvisibleButton(id, ImVec2(12.0f, 12.0f));
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            value -= ImGui::GetIO().MouseDelta.y / graphH * maxV;
            if (value < 0.0f) value = 0.0f;
        }
        dl->AddCircleFilled(pos, (hovered || active) ? 6.0f : 4.5f,
            active ? IM_COL32(255, 220, 120, 255) : IM_COL32(220, 220, 230, 255));
    };
    handle("start", ptStart, m_current.size);
    if (useMid) handle("mid", toPt(0.5f, m_current.sizeMid), m_current.sizeMid);
    handle("end", ptEnd, m_current.sizeEnd);
    ImGui::PopID();

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 6.0f));

    ImGui::SetNextItemWidth(100);
    ImGui::DragFloat("開始 Start##sz", &m_current.size, 0.01f, 0.0f, 20.0f);
    if (useMid)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::DragFloat("中間 Mid##sz", &m_current.sizeMid, 0.01f, 0.0f, 20.0f);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::DragFloat("終了 End##sz", &m_current.sizeEnd, 0.01f, 0.0f, 20.0f);
}

void VfxEditorPanel::RenderWindow(entt::registry& reg, EditorContext& ctx, const std::string& assetsDir)
{
    if (!ctx.showVfxEditor) return;
    if (!m_assetListLoaded) RefreshAssetList(assetsDir);

    // ID を専用にして過去バージョン(旧: 右下タブに強制ドックしていた時代)の imgui.ini 保存状態
    // （小さくドックされた Pos/Size/DockId）を引き継がないようにする。タイトル表示は "###" の前だけ。
    ImGui::SetNextWindowSize(ImVec2(900.0f, 720.0f), ImGuiCond_FirstUseEver);
    // NoDocking: 右下タブ領域(狭い)へ誤ってドッキングされて再び手狭になるのを防ぐ。常にフローティング。
    if (!ImGui::Begin("パーティクルエディタ###VfxEditorPanelFloating", &ctx.showVfxEditor,
                      ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    // 3Dビューポートの上に重なって浮かぶことがあるので、ホバー中はシーンカメラのドラッグ/ホイール
    // 操作が同時に反応しないよう EditorContext 経由で伝える(プレビューのオービット操作漏れ対策)。
    // ThisFrame に書き、読む側は前フレームの確定値を見る。
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                               | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem
                               | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        ctx.floatingToolWindowHoveredThisFrame = true;

    // ---- 左: アセット一覧 ----
    ImGui::BeginChild("##VfxAssetList", ImVec2(170.0f, 0.0f), true);
    ImGui::TextDisabled("VFX アセット");
    ImGui::Separator();
    if (ImGui::Button("＋ 新規 New", ImVec2(-1.0f, 0.0f))) NewAsset();
    if (ImGui::Button("更新 Refresh", ImVec2(-1.0f, 0.0f))) RefreshAssetList(assetsDir);
    ImGui::Separator();
    for (auto& nm : m_assetNames)
    {
        const std::string path = assetsDir + "vfx/" + nm + ".json";
        const bool selected = (m_currentPath == path);
        if (ImGui::Selectable(nm.c_str(), selected))
            LoadAsset(path);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ---- 右: プレビュー + パラメータ編集 ----
    ImGui::BeginChild("##VfxMain", ImVec2(0.0f, 0.0f));

    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputText("名前 Name", m_nameBuf, sizeof(m_nameBuf)))
        m_current.name = m_nameBuf;

    ImGui::SameLine();
    const bool canSave = m_current.name[0] != '\0' && !m_current.name.empty();
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("保存 Save"))
    {
        const std::string path = m_currentPath.empty()
            ? (assetsDir + "vfx/" + m_current.name + ".json") : m_currentPath;
        if (SaveAsset(path))
        {
            m_currentPath = path;
            RefreshAssetList(assetsDir);
            m_statusMsg = "保存しました: " + m_current.name;
            m_statusFlash = 2.0f;
        }
    }
    if (!canSave) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("名前を付けて保存 Save As"))
    {
        const std::string path = assetsDir + "vfx/" + m_current.name + ".json";
        if (SaveAsset(path))
        {
            m_currentPath = path;
            RefreshAssetList(assetsDir);
            m_statusMsg = "保存しました: " + m_current.name;
            m_statusFlash = 2.0f;
        }
    }
    if (!canSave) ImGui::EndDisabled();

    if (!m_currentPath.empty())
    {
        ImGui::SameLine();
        if (ImGui::Button("削除 Delete"))
        {
            std::error_code ec;
            fs::remove(m_currentPath, ec);
            RefreshAssetList(assetsDir);
            NewAsset();
            m_statusMsg = "削除しました";
            m_statusFlash = 2.0f;
        }
    }

    ImGui::Spacing();

    // プレビュー
    ImGui::BeginGroup();
    if (m_gfxInitialized)
    {
        // ImGui::Image は「掴めない」アイテムのため、その上で左ドラッグを始めるとImGui標準の
        // ウィンドウ移動として扱われ、回転操作と一緒に窓ごと動いてしまう。InvisibleButton を
        // 敷いてドラッグをアイテムとして捕捉し、画像は DrawList で重ね描きする(MaterialEditorPanelと同じ対処)。
        const D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGpuHandle(m_previewRT.GetSrvIndex());
        const ImVec2 imgPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##vfxPreviewOrbit", ImVec2(256.0f, 256.0f));
        ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(h.ptr),
            imgPos, ImVec2(imgPos.x + 256.0f, imgPos.y + 256.0f));

        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemActivated())
        {
            m_orbitAnchorX = io.MousePos.x;
            m_orbitAnchorY = io.MousePos.y;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            // 回転量は「アンカーからの位置差」で計算(毎フレームのワープと MouseDelta が相殺して
            // 回転しなくなるため。MaterialEditorPanel と同じ対処)。
            m_camYaw   += (io.MousePos.x - m_orbitAnchorX) * 0.01f;
            m_camPitch += (io.MousePos.y - m_orbitAnchorY) * 0.01f;
            m_camPitch = std::clamp(m_camPitch, -1.5f, 1.5f);

            // カーソルを開始位置へワープ+非表示(無限回転)。
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            io.MousePos = ImVec2(m_orbitAnchorX, m_orbitAnchorY);
            io.WantSetMousePos = true;
        }
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
            m_camDist = std::clamp(m_camDist - io.MouseWheel * 0.4f, 1.0f, 30.0f);
    }
    else
    {
        ImGui::Dummy(ImVec2(256.0f, 256.0f));
    }
    if (ImGui::Button("プレビューをクリア Clear", ImVec2(256.0f, 0.0f)))
        m_previewParticles.Clear();
    ImGui::TextDisabled("ドラッグ:回転 / ホイール:ズーム");
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginChild("##VfxEmission", ImVec2(0.0f, 256.0f), false);
    ImGui::Combo("見た目 Kind", &m_current.kind, kKindNames, IM_ARRAYSIZE(kKindNames));
    const char* blends[] = { "加算 Additive", "アルファ Alpha" };
    ImGui::Combo("合成 Blend", &m_current.blend, blends, IM_ARRAYSIZE(blends));

    {
        static char texBuf[260] = "";
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##vfxtex", "テクスチャ(assetsからの相対パス。空=プロシージャル質感)",
                                 texBuf, sizeof(texBuf));
        if (ImGui::IsItemDeactivatedAfterEdit()) m_current.texturePath = texBuf;
        if (!ImGui::IsItemActive() && m_current.texturePath != texBuf)
        {
            size_t n = m_current.texturePath.size();
            if (n >= sizeof(texBuf)) n = sizeof(texBuf) - 1;
            std::memcpy(texBuf, m_current.texturePath.c_str(), n);
            texBuf[n] = '\0';
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::TextUnformatted("指定すると Kind の数式模様の代わりに画像をビルボード貼り付け表示する。\n"
                                    "色は寿命カーブの頂点色で乗算、アルファは画像のアルファをそのまま使用。");
            ImGui::EndTooltip();
        }
    }
    ImGui::SeparatorText("放出（配置エンティティ用）");
    ImGui::DragFloat("放出レート Rate(/s)", &m_current.rate, 0.5f, 0.0f, 500.0f);
    ImGui::Checkbox("Play開始で放出 PlayOnStart", &m_current.playOnStart);
    ImGui::Checkbox("ループ Looping", &m_current.looping);
    if (!m_current.looping)
        ImGui::DragFloat("継続秒 Duration", &m_current.duration, 0.05f, 0.0f, 60.0f);
    ImGui::DragInt("Luaバースト数 BurstCount", &m_current.burstCount, 1, 1, 2000);
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip())
    { ImGui::TextUnformatted("「Luaコードをコピー」で生成する fx:burst{} が一度に出す粒子数"); ImGui::EndTooltip(); }
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::BeginChild("##VfxScroll", ImVec2(0.0f, -68.0f));
    DrawColorGradient();
    ImGui::Spacing();
    DrawSizeCurve();
    ImGui::SeparatorText("寿命・輝度");
    ImGui::DragFloat("寿命 Life(s)", &m_current.life, 0.01f, 0.01f, 30.0f);
    ImGui::SliderFloat("寿命ばらつき LifeVar", &m_current.lifeVar, 0.0f, 1.0f);
    ImGui::DragFloat("輝度 Intensity", &m_current.intensity, 0.05f, 0.0f, 30.0f);
    ImGui::SeparatorText("動き");
    ImGui::DragFloat3("方向 Dir", &m_current.dir.x, 0.01f);
    ImGui::SliderFloat("拡がり Spread", &m_current.spread, 0.0f, 1.0f);
    ImGui::DragFloat("速度 Speed", &m_current.speed, 0.02f, 0.0f, 50.0f);
    ImGui::SliderFloat("速度ばらつき SpeedVar", &m_current.speedVar, 0.0f, 1.0f);
    ImGui::DragFloat("重力 Gravity", &m_current.gravity, 0.02f, -50.0f, 50.0f);
    ImGui::DragFloat("抵抗 Drag", &m_current.drag, 0.02f, 0.0f, 10.0f);
    ImGui::DragFloat("上向き Up", &m_current.up, 0.02f, 0.0f, 10.0f);
    ImGui::DragFloat("ストレッチ Stretch", &m_current.stretch, 0.02f, 0.0f, 10.0f);
    ImGui::DragFloat("乱流 TurbStrength", &m_current.turbStrength, 0.01f, 0.0f, 10.0f);
    if (m_current.turbStrength > 0.0f)
        ImGui::DragFloat("乱流の細かさ TurbFreq", &m_current.turbFreq, 0.01f, 0.01f, 10.0f);
    ImGui::SeparatorText("特殊効果");
    ImGui::SliderFloat("画面歪み Distort", &m_current.distort, 0.0f, 3.0f);
    ImGui::Checkbox("ライト放出 Light", &m_current.light);
    if (m_current.light)
        ImGui::DragFloat("光の距離 LightRange", &m_current.lightRange, 0.05f, 0.1f, 50.0f);
    ImGui::SliderFloat("明滅 Flicker", &m_current.flicker, 0.0f, 1.0f);
    if (m_current.flicker > 0.0f)
        ImGui::DragFloat("明滅の速さ FlickerFreq", &m_current.flickerFreq, 0.2f, 0.1f, 60.0f);
    ImGui::EndChild();

    ImGui::Separator();
    const bool canApply = ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity)
                        && reg.all_of<ParticleEmitter>(ctx.selectedEntity);
    if (!canApply) ImGui::BeginDisabled();
    if (ImGui::Button("選択エンティティへ適用")) ApplyToSelected(reg, ctx);
    if (!canApply) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("新規エンティティとして配置")) SpawnEntity(reg, ctx);
    ImGui::SameLine();
    if (ImGui::Button("Luaコードをコピー"))
    {
        ImGui::SetClipboardText(BuildLuaSnippet().c_str());
        m_statusMsg = "Luaコードをクリップボードにコピーしました";
        m_statusFlash = 2.0f;
    }
    if (m_statusFlash > 0.0f)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f), "%s", m_statusMsg.c_str());
        m_statusFlash -= ImGui::GetIO().DeltaTime;
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace dx12e
