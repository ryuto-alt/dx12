#include "editor/UiWidgets.h"
#include "editor/panels/FoliagePanel.h"

#include "core/Logger.h"
#include "core/PathResolver.h"
#include "ecs/Components.h"
#include "editor/EditorContext.h"
#include "editor/FoliageOps.h"
#include "editor/PropertyGrid.h"
#include "editor/ScenePick.h"
#include "editor/UndoSystem.h"
#include "renderer/Camera.h"
#include "renderer/foliage/FoliageLayerOps.h"
#include "scene/Scene.h"
#include "terrain/HeightField.h"

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include "gui/ImGuizmo.h"

#include <DirectXMath.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

using namespace DirectX;

namespace dx12e
{
namespace
{

// ブラシ 1 ストローク（押下〜離す）を 1 エントリの Undo にする。値でコピーした FoliageLayer（_set は COW のポインタ）を前後で持つ。
// ★Undo / Redo のあと .dxfoliage を書き直すよう _needsSave を立てる（ディスクの実体を画面と一致させる）。
class FoliageStrokeCommand final : public IUndoCommand
{
public:
    FoliageStrokeCommand(entt::registry* reg, entt::entity e, const FoliageLayer& before, const FoliageLayer& after, const char* label)
        : m_reg(reg), m_e(e), m_before(before), m_after(after), m_label(label) {}
    void Undo() override { Apply(m_before); }
    void Redo() override { Apply(m_after); }
    const char* GetName() const override { return m_label; }

private:
    void Apply(const FoliageLayer& v)
    {
        if (!m_reg->valid(m_e) || !m_reg->all_of<FoliageLayer>(m_e)) return;
        FoliageLayer& l = m_reg->get<FoliageLayer>(m_e);
        l._set = v._set;
        l._loadTried = true;
        l._needsSave = true;
    }
    entt::registry* m_reg;
    entt::entity m_e;
    FoliageLayer m_before, m_after;
    const char* m_label;
};

struct FoliageToolState
{
    Scene* scene = nullptr;
    EditorContext* ctx = nullptr;
    bool handlerRegistered = false;
    entt::entity activeLayer = entt::null;
    entt::entity prevSelected = entt::null;
    // 散布
    int surfaceIdx = 0;                 // 0=auto 1=terrain 2=flat 3=raycast
    float flatHeight = 0.0f;
    foliage::ScatterParams sp;
    bool replace = true;
    char lastMessage[256] = {};
    // ブラシ
    bool brushEnabled = false;
    int brushMode = 0;                  // 0=追加 1=削除
    float radius = 6.0f;
    float density = 1.0f;
    float erasePerStamp = 0.5f;
    // ストローク
    bool strokeActive = false;
    FoliageLayer strokeBefore;
    XMFLOAT2 lastStamp{1e30f, 1e30f};
    std::chrono::steady_clock::time_point lastStampTime{};
    u32 strokeIndex = 0;
    u32 strokeSeed = 1;
};

FoliageToolState& State()
{
    static FoliageToolState s;
    return s;
}

const char* const kSurfaceLabels[] = {"自動（地形があれば地形）", "地形", "平面（高さ指定）", "メッシュ表面（真下レイ）"};
const char* const kSurfaceModes[] = {"auto", "terrain", "flat", "raycast"};
const char* const kBrushLabels[] = {"追加 Add", "削除 Erase"};

entt::entity FindActiveLayer(entt::registry& reg, const EditorContext& ctx, entt::entity remembered)
{
    if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity) && reg.all_of<FoliageLayer>(ctx.selectedEntity))
        return ctx.selectedEntity;
    if (remembered != entt::null && reg.valid(remembered) && reg.all_of<FoliageLayer>(remembered)) return remembered;
    for (auto [e, l] : reg.view<FoliageLayer>().each()) return e;
    return entt::null;
}

bool WorldToScreen(const Camera& cam, const XMFLOAT3& world, f32 vpX, f32 vpY, f32 vpW, f32 vpH, ImVec2& out)
{
    const XMVECTOR clip = XMVector4Transform(XMVectorSet(world.x, world.y, world.z, 1.0f), cam.GetViewProjMatrix());
    const f32 w = XMVectorGetW(clip);
    if (w <= 1e-4f) return false;
    out.x = vpX + (XMVectorGetX(clip) / w * 0.5f + 0.5f) * vpW;
    out.y = vpY + (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * vpH;
    return true;
}

// カーソルのワールド位置（地形があれば地形の高さ配列へ、無ければシーンのメッシュへ真下ではなくカメラのレイを当てる）。
bool PickSurface(FoliageToolState& s, const ViewportInput& in, XMFLOAT3& hitWorld)
{
    entt::registry& reg = s.scene->GetRegistry();
    for (auto [te, t] : reg.view<Terrain>().each())
    {
        if (!t._hf || !t._hf->IsValid()) continue;
        const XMFLOAT3 tpos = reg.all_of<Transform>(te) ? reg.get<Transform>(te).position : XMFLOAT3{0, 0, 0};
        const XMFLOAT3 lo{in.rayOrigin.x - tpos.x, in.rayOrigin.y - tpos.y, in.rayOrigin.z - tpos.z};
        f32 tHit = 0.0f;
        if (t._hf->Raycast(lo, in.rayDir, 100000.0f, tHit))
        {
            hitWorld = {in.rayOrigin.x + in.rayDir.x * tHit, in.rayOrigin.y + in.rayDir.y * tHit, in.rayOrigin.z + in.rayDir.z * tHit};
            return true;
        }
    }
    ScenePickOptions opt;
    opt.includeNonMesh = false;
    const auto hits = RaycastSceneRay(reg, s.ctx->drawItems, in.rayOrigin, in.rayDir, 100000.0f, opt);
    for (const auto& h : hits)
    {
        if (h.entity == s.activeLayer || h.isIcon) continue;
        hitWorld = h.worldPos;
        return true;
    }
    return false;
}

void DrawCursor(const ViewportInput& in, const XMFLOAT3& center, f32 radius, ImU32 col)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
    if (!dl || !in.camera) return;
    constexpr int kSeg = 48;
    ImVec2 pts[kSeg];
    for (int i = 0; i < kSeg; ++i)
    {
        const f32 a = static_cast<f32>(i) * (6.28318530718f / static_cast<f32>(kSeg));
        const XMFLOAT3 w{center.x + std::cos(a) * radius, center.y, center.z + std::sin(a) * radius};
        if (!WorldToScreen(*in.camera, w, in.vpX, in.vpY, in.vpW, in.vpH, pts[i])) return;
    }
    dl->AddPolyline(pts, kSeg, col, ImDrawFlags_Closed, ui::PxF(2.0f));
}

foliage::ScatterParams BrushParams(const FoliageToolState& s)
{
    foliage::ScatterParams p = s.sp;
    p.density = s.density;
    return p;
}

void ApplyStamp(FoliageToolState& s, entt::registry& reg, const XMFLOAT3& hit)
{
    if (s.activeLayer == entt::null || !reg.valid(s.activeLayer) || !reg.all_of<FoliageLayer>(s.activeLayer)) return;
    FoliageLayer& layer = reg.get<FoliageLayer>(s.activeLayer);
    const foliage_ops::LayerXform lx = foliage_ops::MakeLayerXform(reg, s.activeLayer);
    const std::vector<std::pair<f32, f32>> centers{{hit.x, hit.z}};
    foliage_ops::EditResult er;
    if (s.brushMode == 1)
    {
        er = foliage_ops::BrushErase(layer, lx, centers, s.radius, s.erasePerStamp, s.strokeSeed + s.strokeIndex * 31u);
    }
    else
    {
        foliage_ops::SurfaceRequest rq;
        rq.mode = kSurfaceModes[std::clamp(s.surfaceIdx, 0, 3)];
        rq.flatHeight = s.flatHeight;
        rq.hasRegion = true;
        rq.region = {hit.x - s.radius, hit.z - s.radius, hit.x + s.radius, hit.z + s.radius};
        foliage_ops::BuiltSurface bs;
        std::string err;
        if (!foliage_ops::BuildSurface(reg, s.ctx->drawItems, s.activeLayer, rq, bs, err)) { std::snprintf(s.lastMessage, sizeof(s.lastMessage), "%s", err.c_str()); return; }
        foliage::ScatterParams p = BrushParams(s);
        p.seed = s.strokeSeed + s.strokeIndex * 7919u;
        er = foliage_ops::BrushAdd(layer, lx, p, centers, s.radius, bs.fn);
    }
    ++s.strokeIndex;
    std::snprintf(s.lastMessage, sizeof(s.lastMessage), "%s: %u → %u 個（%+d）", s.brushMode == 1 ? "削除" : "追加", er.before, er.after,
                  static_cast<int>(er.after) - static_cast<int>(er.before));
}

void EndStroke(FoliageToolState& s, entt::registry& reg)
{
    if (!s.strokeActive) return;
    s.strokeActive = false;
    if (s.activeLayer == entt::null || !reg.valid(s.activeLayer) || !reg.all_of<FoliageLayer>(s.activeLayer)) return;
    FoliageLayer& layer = reg.get<FoliageLayer>(s.activeLayer);
    if (layer._set.get() == s.strokeBefore._set.get()) return;   // 何も変わらなかった
    // ストロークが終わったところで .dxfoliage を書く（ドラッグ中は書かない）
    std::string err;
    const NameTag* nt = reg.try_get<NameTag>(s.activeLayer);
    foliage::FlushSidecar(layer, nt ? nt->name : std::string("Foliage"), PathResolver::AssetsDir(), &err, true);
    s.ctx->undoSystem.PushCommand(std::make_unique<FoliageStrokeCommand>(&reg, s.activeLayer, s.strokeBefore, layer,
                                                                          s.brushMode == 1 ? "Foliage Erase" : "Foliage Paint"));
}

bool HandleViewportBrush(const ViewportInput& in)
{
    FoliageToolState& s = State();
    if (!s.scene || !s.ctx || !in.camera) return false;
    EditorContext& ctx = *s.ctx;
    if (!ctx.showFoliageTool || !s.brushEnabled) return false;
    if (ImGuizmo::IsUsing()) return false;
    entt::registry& reg = s.scene->GetRegistry();
    s.activeLayer = FindActiveLayer(reg, ctx, s.activeLayer);
    if (s.activeLayer == entt::null) return false;

    // [ ] で半径
    if (in.inViewport)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) s.radius = std::max(0.3f, s.radius * 0.85f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) s.radius = std::min(500.0f, s.radius * 1.18f);
    }
    XMFLOAT3 hit{};
    const bool hasHit = in.inViewport && PickSurface(s, in, hit);
    if (hasHit) DrawCursor(in, hit, s.radius, s.brushMode == 1 ? IM_COL32(255, 110, 92, 230) : IM_COL32(120, 230, 130, 230));

    if (in.pressed && hasHit)
    {
        s.strokeActive = true;
        s.strokeBefore = reg.get<FoliageLayer>(s.activeLayer);
        foliage::EnsureLoaded(reg.get<FoliageLayer>(s.activeLayer));
        s.strokeBefore = reg.get<FoliageLayer>(s.activeLayer);   // 読込後の実体を「前」にする
        s.strokeIndex = 0;
        s.strokeSeed = static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count() & 0x7FFFFFFF);
        s.lastStamp = {1e30f, 1e30f};
        s.lastStampTime = {};
    }
    if (s.strokeActive && hasHit && (in.pressed || in.dragging))
    {
        const f32 dx = hit.x - s.lastStamp.x, dz = hit.z - s.lastStamp.y;
        const f32 minMove = s.radius * 0.35f;
        const auto now = std::chrono::steady_clock::now();
        const bool timeOk = (now - s.lastStampTime) > std::chrono::milliseconds(90);   // 巨大なレイヤーで毎フレーム作り直さない
        if ((dx * dx + dz * dz >= minMove * minMove) && timeOk)
        {
            ApplyStamp(s, reg, hit);
            s.lastStamp = {hit.x, hit.z};
            s.lastStampTime = now;
        }
    }
    bool consume = false;
    if (in.pressed && hasHit) consume = true;
    if (s.strokeActive && (in.dragging || in.released)) consume = true;
    if (in.released) EndStroke(s, reg);
    return consume;
}

} // namespace

namespace FoliagePanel
{

void Render(Scene& scene, EditorContext& ctx, const std::string& assetsDir)
{
    FoliageToolState& s = State();
    s.scene = &scene;
    s.ctx = &ctx;
    if (!s.handlerRegistered)
    {
        s.handlerRegistered = true;
        ctx.viewportToolHandlers.push_back(&HandleViewportBrush);
    }
    if (!ctx.showFoliageTool) return;
    entt::registry& reg = scene.GetRegistry();
    s.activeLayer = FindActiveLayer(reg, ctx, s.activeLayer);

    ImGui::SetNextWindowSize(ui::Px(420.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("植生ツール###FoliageToolFloating", &ctx.showFoliageTool, ImGuiWindowFlags_NoDocking))
    {
        ImGui::End();
        return;
    }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        ctx.floatingToolWindowHoveredThisFrame = true;

    // ---------------- 対象レイヤー ----------------
    if (ui::CollapsingHeader("対象レイヤー", ImGuiTreeNodeFlags_DefaultOpen))
    {
        std::vector<entt::entity> layers;
        for (auto [e, l] : reg.view<FoliageLayer>().each()) layers.push_back(e);
        if (layers.empty())
        {
            ImGui::TextDisabled("FoliageLayer がありません。");
            if (ImGui::Button("＋ 植生レイヤーを作る"))
            {
                entt::entity ne = reg.create();
                reg.emplace<NameTag>(ne, NameTag{"Foliage"});
                reg.emplace<Transform>(ne, Transform{});
                reg.emplace<FoliageLayer>(ne);
                ctx.selectedEntity = ne;
                s.activeLayer = ne;
            }
        }
        else
        {
            std::vector<std::string> names;
            std::vector<const char*> labels;
            int cur = 0;
            for (size_t i = 0; i < layers.size(); ++i)
            {
                const NameTag* nt = reg.try_get<NameTag>(layers[i]);
                names.push_back(nt ? nt->name : std::string("(no name)"));
                if (layers[i] == s.activeLayer) cur = static_cast<int>(i);
            }
            for (const auto& n : names) labels.push_back(n.c_str());
            if (pg::Begin("##FoliageTarget"))
            {
                if (pg::Combo("レイヤー", &cur, labels.data(), static_cast<int>(labels.size()))) { s.activeLayer = layers[static_cast<size_t>(cur)]; ctx.selectedEntity = s.activeLayer; }
                pg::End();
            }
            if (s.activeLayer != entt::null && reg.valid(s.activeLayer) && reg.all_of<FoliageLayer>(s.activeLayer))
            {
                FoliageLayer& l = reg.get<FoliageLayer>(s.activeLayer);
                const u32 n = foliage::InstanceCount(l);
                ImGui::Text("インスタンス: %u 個 / %.1f MB（32 B/個）", n, static_cast<double>(n) * 32.0 / (1024.0 * 1024.0));
                if (l._set && l._set->Count() > 0)
                    ImGui::TextDisabled("範囲 X[%.0f..%.0f] Z[%.0f..%.0f]  チャンク %zu", l._set->boundsMin[0], l._set->boundsMax[0],
                                        l._set->boundsMin[2], l._set->boundsMax[2], l._set->chunks.size());
                ImGui::TextDisabled("ファイル: %s%s", l.instancePath.empty() ? "(未保存)" : l.instancePath.c_str(), l._needsSave ? "（未書き出し）" : "");
                if (pg::Begin("##FoliageModels"))
                {
                    pg::InputTextStr("モデル（種別 0）", l.variant0, nullptr, "「;」区切りの LOD 列（LOD0 が最詳細。assets 相対）例: foliage/tree_lod0.glb;foliage/tree_lod1.glb;foliage/tree_lod2.glb");
                    pg::End();
                }
            }
        }
    }
    const bool haveLayer = s.activeLayer != entt::null && reg.valid(s.activeLayer) && reg.all_of<FoliageLayer>(s.activeLayer);

    // ---------------- 散布 ----------------
    if (ui::CollapsingHeader("散布（ポアソンディスク）", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (pg::Begin("##FoliageScatter"))
        {
            pg::Combo("表面", &s.surfaceIdx, kSurfaceLabels, 4, "地形の高さ配列 / 平面 / シーンのメッシュへ真下レイ");
            if (s.surfaceIdx == 2) pg::Float("平面の高さ", &s.flatHeight, 0.1f, -1000.0f, 1000.0f, "%.2f");
            pg::Float("密度 /m²", &s.sp.density, 0.01f, 0.0f, 100.0f, "%.3f", nullptr, "平均個数/m²。最小間隔があるときは最大充填のうち何割か");
            pg::Float("最小間隔 m", &s.sp.minSpacing, 0.02f, 0.0f, 100.0f, "%.2f", nullptr, "0 = 一様ランダム / >0 = ポアソンディスク（既存とも守る）");
            pg::Float("スケール最小", &s.sp.scaleMin, 0.01f, 0.05f, 20.0f, "%.2f");
            pg::Float("スケール最大", &s.sp.scaleMax, 0.01f, 0.05f, 20.0f, "%.2f");
            pg::SliderFloat("法線への傾き", &s.sp.tiltAlign, 0.0f, 1.0f, "%.2f");
            pg::SliderFloat("傾斜の上限 °", &s.sp.maxSlopeDeg, 0.0f, 90.0f, "%.0f", nullptr, "これより急な斜面には置かない");
            pg::Float("高度の下限", &s.sp.minHeight, 0.5f, -10000.0f, 10000.0f, "%.1f");
            pg::Float("高度の上限", &s.sp.maxHeight, 0.5f, -10000.0f, 10000.0f, "%.1f");
            pg::SliderInt("スプラット層", &s.sp.splatLayer, -1, 3, nullptr, "-1 = 使わない / 0..3 = その地形テクスチャ層の重みで採否を決める");
            pg::SliderFloat("色ばらつき", &s.sp.colorVariation, 0.0f, 1.0f, "%.2f");
            {
                int vc = static_cast<int>(s.sp.variantCount);
                if (pg::SliderInt("種別の数", &vc, 1, 4, nullptr, "モデル（種別 1..3）は Inspector で指定する")) s.sp.variantCount = static_cast<u32>(vc);
            }
            int seed = static_cast<int>(s.sp.seed);
            if (pg::Int("乱数シード", &seed, 1.0f, 0, 2000000000)) s.sp.seed = static_cast<u32>(seed);
            pg::Checkbox("既存を置き換える", &s.replace, "OFF にすると追加（既存との最小間隔も守る）");
            pg::End();
        }
        ImGui::BeginDisabled(!haveLayer);
        if (ui::PrimaryButton("全面へ散布"))
        {
            FoliageLayer& layer = reg.get<FoliageLayer>(s.activeLayer);
            const FoliageLayer before = layer;
            foliage_ops::SurfaceRequest rq;
            rq.mode = kSurfaceModes[std::clamp(s.surfaceIdx, 0, 3)];
            rq.flatHeight = s.flatHeight;
            foliage_ops::BuiltSurface bs;
            std::string err;
            if (!foliage_ops::BuildSurface(reg, ctx.drawItems, s.activeLayer, rq, bs, err) || !bs.regionKnown)
                std::snprintf(s.lastMessage, sizeof(s.lastMessage), "%s", err.empty() ? "領域が決まりません（地形を作るか、平面 / メッシュは MCP の region 指定を使う）" : err.c_str());
            else
            {
                const foliage_ops::LayerXform lx = foliage_ops::MakeLayerXform(reg, s.activeLayer);
                const foliage_ops::EditResult er = foliage_ops::ScatterLayer(layer, lx, s.sp, bs.region, bs.fn, s.replace);
                const NameTag* nt = reg.try_get<NameTag>(s.activeLayer);
                std::string ferr;
                foliage::FlushSidecar(layer, nt ? nt->name : std::string("Foliage"), assetsDir, &ferr, true);
                ctx.undoSystem.PushCommand(std::make_unique<FoliageStrokeCommand>(&reg, s.activeLayer, before, layer, "Foliage Scatter"));
                std::snprintf(s.lastMessage, sizeof(s.lastMessage), "散布: %u 個（候補 %u / 傾斜で除外 %u / 高度 %u / スプラット %u）%.0f ms",
                              er.after, er.stats.candidates, er.stats.rejectedSlope, er.stats.rejectedHeight, er.stats.rejectedSplat, er.msScatter + er.msChunks);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("全部消す") && haveLayer)
        {
            FoliageLayer& layer = reg.get<FoliageLayer>(s.activeLayer);
            const FoliageLayer before = layer;
            foliage::ReplaceSet(layer, std::make_shared<foliage::FoliageInstanceSet>());
            const NameTag* nt = reg.try_get<NameTag>(s.activeLayer);
            std::string ferr;
            foliage::FlushSidecar(layer, nt ? nt->name : std::string("Foliage"), assetsDir, &ferr, true);
            ctx.undoSystem.PushCommand(std::make_unique<FoliageStrokeCommand>(&reg, s.activeLayer, before, layer, "Foliage Clear"));
            std::snprintf(s.lastMessage, sizeof(s.lastMessage), "全部消しました");
        }
        ImGui::SameLine();
        if (ImGui::Button("保存") && haveLayer)
        {
            const NameTag* nt = reg.try_get<NameTag>(s.activeLayer);
            std::string ferr;
            const bool ok = foliage::FlushSidecar(reg.get<FoliageLayer>(s.activeLayer), nt ? nt->name : std::string("Foliage"), assetsDir, &ferr, true);
            std::snprintf(s.lastMessage, sizeof(s.lastMessage), ok ? "保存しました" : "保存に失敗: %s", ferr.c_str());
        }
        ImGui::EndDisabled();
    }

    // ---------------- ブラシ ----------------
    if (ui::CollapsingHeader("ブラシ（円・追加 / 削除）", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (pg::Begin("##FoliageBrush"))
        {
            pg::Checkbox("ブラシを有効にする", &s.brushEnabled, "ONの間、3D ビューの左ドラッグで塗る（選択 / ギズモより優先）。[ ] で半径");
            pg::Combo("モード", &s.brushMode, kBrushLabels, 2);
            pg::Float("半径 m", &s.radius, 0.1f, 0.3f, 500.0f, "%.1f");
            if (s.brushMode == 0)
                pg::Float("密度 /m²", &s.density, 0.01f, 0.0f, 100.0f, "%.3f", nullptr, "追加の密度。最小間隔は上の散布の値を使う（既存とも守る）");
            else
                pg::SliderFloat("消す割合", &s.erasePerStamp, 0.0f, 1.0f, "%.2f", nullptr, "1 回のスタンプで円の中の何割を消すか");
            pg::End();
        }
        ImGui::TextDisabled("3D ビューを左ドラッグ。同じ場所を繰り返すと密度が上がる / 減る。ストローク単位で Ctrl+Z。");
    }

    // ---------------- 風 ----------------
    if (ui::CollapsingHeader("シーンの風", ImGuiTreeNodeFlags_DefaultOpen))
    {
        foliage::SceneWind& w = scene.GetWind();
        if (pg::Begin("##FoliageWind"))
        {
            pg::Checkbox("風を有効にする", &w.enabled);
            pg::Float("風向き °", &w.directionDeg, 1.0f, -3600.0f, 3600.0f, "%.0f", nullptr, "風が吹いていく向き。0° = +X、90° = +Z");
            pg::Float("風速 m/s", &w.speed, 0.05f, 0.0f, 60.0f, "%.2f", nullptr, "0 で無風");
            pg::SliderFloat("突風", &w.gustStrength, 0.0f, 1.0f, "%.2f");
            pg::Float("突風の速さ", &w.gustFrequency, 0.01f, 0.0f, 5.0f, "%.2f");
            pg::SliderFloat("乱れ", &w.turbulence, 0.0f, 1.0f, "%.2f");
            pg::Float("位相オフセット s", &w.phaseOffset, 0.05f, -100000.0f, 100000.0f, "%.2f", nullptr, "決定論キャプチャ（screenshot_final）で風の位相を選ぶのに使う");
            pg::End();
        }
        ImGui::TextDisabled("幹の曲げ / 葉のはばたきの強さは各レイヤーの Inspector（windBend / windFlutter）。");
    }

    if (s.lastMessage[0]) ImGui::TextWrapped("%s", s.lastMessage);
    ImGui::End();
}

} // namespace FoliagePanel

} // namespace dx12e
