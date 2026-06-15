#include "editor/panels/InspectorPanel.h"
#include "editor/EditorContext.h"
#include "editor/UndoSystem.h"
#include "ecs/Components.h"
#include "renderer/Camera.h"
#include "renderer/Material.h"
#include "renderer/Mesh.h"
#include "audio/AudioSystem.h"
#include "physics/PhysicsDebugRenderer.h"
#include "core/GameClock.h"
#include "scene/Scene.h"
#include "animation/Skeleton.h"
#include "animation/AnimationClip.h"
#include "animation/Animator.h"
#include "animation/NodeGraph.h"
#include "animation/NodeAnimationClip.h"
#include "animation/NodeAnimator.h"
#include "animation/SkinningBuffer.h"
#include "scripting/ScriptEngine.h"

#include <filesystem>
#include <algorithm>
#include <cstring>

#pragma warning(push)
#pragma warning(disable: 4100)
#include <Windows.h>
#include <shellapi.h>
#pragma warning(pop)

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#pragma warning(pop)

#include <DirectXMath.h>

namespace
{

// CollapsingHeader の頭に種別アイコンを重ね描きするヘルパ。
// tex==0 または icons==null なら通常のヘッダ。アイコン分だけラベル先頭に空白を入れて重ねる。
bool IconHeader(const dx12e::EditorUiIcons* ic, dx12e::u64 tex, const char* label,
                ImGuiTreeNodeFlags flags = 0)
{
    if (!ic || !tex)
        return ImGui::CollapsingHeader(label, flags);

    const float h = ImGui::GetTextLineHeight();
    const float spaceW = ImGui::CalcTextSize(" ").x;
    const int pad = (spaceW > 0.0f) ? static_cast<int>((h + 6.0f) / spaceW) + 1 : 3;
    std::string padded(static_cast<size_t>(pad), ' ');
    padded += label;

    const bool open = ImGui::CollapsingHeader(padded.c_str(), flags);

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    const float cy = (mn.y + mx.y) * 0.5f;
    const float x  = mn.x + ImGui::GetTreeNodeToLabelSpacing();
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(tex),
        ImVec2(x, cy - h * 0.5f), ImVec2(x + h, cy + h * 0.5f));
    return open;
}

// エンティティの構成から代表アイコンを選ぶ（Inspector 上部の見出し用）
dx12e::u64 PickEntityIcon(entt::registry& reg, entt::entity e, const dx12e::EditorUiIcons& ic)
{
    using namespace dx12e;
    if (reg.all_of<CameraComponent>(e))                       return ic.entCamera;
    if (reg.any_of<PointLight, DirectionalLight, SpotLight>(e)) return ic.entLight;
    if (reg.all_of<MeshRenderer>(e))                          return ic.entMesh;
    if (reg.all_of<AudioSource>(e))                           return ic.entAudio;
    if (reg.any_of<RigidBody, BoxCollider, SphereCollider,
                   CapsuleCollider, ConvexHullCollider>(e))   return ic.entPhysics;
    if (reg.all_of<LuaScript>(e))                             return ic.entScript;
    return ic.entEmpty;
}

void DrawLuaScriptSection(entt::registry& reg,
                          entt::entity e,
                          dx12e::ScriptEngine* scriptEngine,
                          const std::string& assetsDir)
{
    const bool hasLua = reg.all_of<dx12e::LuaScript>(e);

    bool open = ImGui::CollapsingHeader("Lua Script",
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    // ヘッダ右の状態アイコン: 付いてたら緑チェック、無ければグレー
    ImGui::SameLine(ImGui::GetWindowWidth() - 50.0f);
    if (hasLua)
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "ATTACHED");
    else
        ImGui::TextDisabled("(none)");

    if (!open) return;

    if (!hasLua)
    {
        ImGui::TextDisabled("Drop a .lua file from AssetBrowser onto this entity");
        ImGui::TextDisabled("(Hierarchy row or this Inspector panel)");
        return;
    }

    auto& ls = reg.get<dx12e::LuaScript>(e);

    // Script path (read-only)
    char pathBuf[256];
    std::memset(pathBuf, 0, sizeof(pathBuf));
    strncpy_s(pathBuf, sizeof(pathBuf), ls.scriptPath.c_str(), _TRUNCATE);
    ImGui::InputText("Script##LuaScript", pathBuf, sizeof(pathBuf),
                     ImGuiInputTextFlags_ReadOnly);

    ImGui::Checkbox("Enabled##LuaScript", &ls.enabled);
    ImGui::SameLine();
    ImGui::TextColored(
        ls.started ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        ls.started ? "RUNNING" : "IDLE");

    if (ImGui::Button("Reload##LuaScript"))
    {
        if (scriptEngine) scriptEngine->ReloadScript(e);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open in Editor##LuaScript"))
    {
        namespace fs = std::filesystem;
        fs::path abs = fs::path(assetsDir) / ls.scriptPath;
        ShellExecuteA(nullptr, "open", abs.string().c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Detach##LuaScript"))
    {
        if (scriptEngine) scriptEngine->DetachScriptFromEntity(e);
        return;
    }

    if (ls.loadError)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                           "Load error (see log)");
    }
}

} // anonymous namespace

namespace dx12e
{

namespace
{

// ── コンポーネント編集 Undo 追跡ヘルパー ──
// 描画前に BeginEdit でスナップショットを取り、描画後に EndEdit で
// 「即時変更 (checkbox/combo)」または「ドラッグ終了」を検出して Undo に積む
template<typename T>
void BeginEdit(entt::registry& reg, entt::entity e, InspectorPanel::EditState<T>& state)
{
    if (!state.editing)
        state.snapshot = reg.get<T>(e);
}

template<typename T>
void EndEdit(entt::registry& reg, EditorContext& ctx, entt::entity e,
             InspectorPanel::EditState<T>& state,
             bool changed, bool active, const char* name)
{
    auto push = [&]()
    {
        const T& cur = reg.get<T>(e);
        if (std::memcmp(&state.snapshot, &cur, sizeof(T)) != 0)
        {
            ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<T>>(
                &reg, e, state.snapshot, cur, name));
        }
    };

    if (active)
    {
        state.editing = true;          // ドラッグ継続中
    }
    else if (changed)
    {
        push();                        // 即時変更 (checkbox/combo/単発編集)
        state.editing = false;
    }
    else if (state.editing && !ImGui::IsAnyItemActive())
    {
        push();                        // ドラッグ終了
        state.editing = false;
    }
}

// ── ヘッダ右クリックでコンポーネント削除（Undo 対応）。削除したら true ──
template<typename T>
bool ComponentRemoveMenu(entt::registry& reg, EditorContext& ctx,
                         entt::entity e, const char* name)
{
    bool removed = false;
    ImGui::PushID(name);
    if (ImGui::BeginPopupContextItem("##RemoveComponent"))
    {
        // コンポーネント削除
        if (ImGui::MenuItem("\xe3\x82\xb3\xe3\x83\xb3\xe3\x83\x9d\xe3\x83\xbc\xe3\x83\x8d\xe3\x83\xb3\xe3\x83\x88\xe5\x89\x8a\xe9\x99\xa4"))
        {
            ctx.undoSystem.PushCommand(std::make_unique<RemoveComponentCommand<T>>(
                &reg, e, reg.get<T>(e), name));
            reg.remove<T>(e);
            removed = true;
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return removed;
}

// ── 光の照らす方向を編集する UI（DragFloat3 + プリセット + 正規化） ──
// changed を返し、ドラッグ中は active を立てる（Undo 追跡用）。
bool DirectionEditor(const char* id, DirectX::XMFLOAT3& dir, bool& active)
{
    bool changed = false;
    ImGui::PushID(id);
    ImGui::TextUnformatted("照らす方向 Direction");
    changed |= ImGui::DragFloat3("##dir", &dir.x, 0.01f, -1.0f, 1.0f, "%.2f");
    active  |= ImGui::IsItemActive();

    struct Preset { const char* label; float x, y, z; };
    static const Preset presets[] = {
        {"真下", 0.0f, -1.0f,  0.0f},
        {"斜め", -0.4f, -1.0f, -0.4f},
        {"横",   1.0f,  0.0f,  0.0f},
        {"前",   0.0f,  0.0f,  1.0f},
    };
    for (int i = 0; i < 4; ++i)
    {
        if (i > 0) ImGui::SameLine();
        if (ImGui::SmallButton(presets[i].label))
        {
            dir = {presets[i].x, presets[i].y, presets[i].z};
            changed = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("正規化"))
    {
        DirectX::XMVECTOR v = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&dir));
        DirectX::XMStoreFloat3(&dir, v);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

// ── Add Component メニュー項目（未所持のものだけ表示、Undo 対応） ──
template<typename T>
void AddComponentMenuItem(entt::registry& reg, EditorContext& ctx,
                          entt::entity e, const char* label, const T& initial = T{})
{
    if (reg.all_of<T>(e)) return;
    if (ImGui::MenuItem(label))
    {
        reg.emplace<T>(e, initial);
        ctx.undoSystem.PushCommand(std::make_unique<AddComponentCommand<T>>(
            &reg, e, initial, label));
    }
}

} // anonymous namespace

void InspectorPanel::Render(entt::registry& reg,
                            EditorContext& ctx,
                            Camera* camera,
                            AudioSystem* audioSystem,
                            PhysicsDebugRenderer* physicsDebugRenderer,
                            bool& physicsDebugDraw,
                            bool& useVsync,
                            i32& shadowQualityIndex,
                            u32& shadowMapSize,
                            bool& shadowMapDirty,
                            GameClock* clock,
                            Scene* scene)
{
    ImGui::Begin("\xe3\x82\xa4\xe3\x83\xb3\xe3\x82\xb9\xe3\x83\x9a\xe3\x82\xaf\xe3\x82\xbf\xe3\x83\xbc");  // Inspector

    // --- Selected Entity properties ---
    if (ctx.selectedEntity != entt::null && reg.valid(ctx.selectedEntity))
    {
        const EditorUiIcons* ic = ctx.icons;

        // NameTag（種別アイコン + 名前）
        if (reg.all_of<NameTag>(ctx.selectedEntity))
        {
            auto& tag = reg.get<NameTag>(ctx.selectedEntity);
            if (ic)
            {
                u64 tex = PickEntityIcon(reg, ctx.selectedEntity, *ic);
                if (tex)
                {
                    float s = ImGui::GetTextLineHeight() * 1.3f;
                    ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(s, s));
                    ImGui::SameLine(0.0f, 6.0f);
                    ImGui::AlignTextToFramePadding();
                }
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.39f, 0.58f, 0.93f, 1.0f));
            ImGui::Text("%s", tag.name.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        // Transform
        if (reg.all_of<Transform>(ctx.selectedEntity))
        {
            if (IconHeader(ic, ic ? ic->entEmpty : 0, "Transform", ImGuiTreeNodeFlags_DefaultOpen))
            {
                auto& t = reg.get<Transform>(ctx.selectedEntity);

                // 編集開始前にスナップショットを取る（毎フレーム、非編集中のみ）
                if (!m_transformEditing)
                    m_transformSnapshot = t;

                ImGui::DragFloat3("Position", &t.position.x, 0.1f);
                bool posActive = ImGui::IsItemActive();
                ImGui::DragFloat3("Rotation", &t.rotation.x, 1.0f);
                bool rotActive = ImGui::IsItemActive();
                ImGui::DragFloat3("Scale",    &t.scale.x,    0.01f);
                bool sclActive = ImGui::IsItemActive();

                bool anyActive = posActive || rotActive || sclActive;
                if (anyActive)
                    m_transformEditing = true;
            }

            // Transform 編集中 → 全ウィジェットが非アクティブになったら Undo に積む
            if (m_transformEditing && !ImGui::IsAnyItemActive())
            {
                auto& t = reg.get<Transform>(ctx.selectedEntity);
                bool changed =
                    m_transformSnapshot.position.x != t.position.x ||
                    m_transformSnapshot.position.y != t.position.y ||
                    m_transformSnapshot.position.z != t.position.z ||
                    m_transformSnapshot.rotation.x != t.rotation.x ||
                    m_transformSnapshot.rotation.y != t.rotation.y ||
                    m_transformSnapshot.rotation.z != t.rotation.z ||
                    m_transformSnapshot.scale.x    != t.scale.x ||
                    m_transformSnapshot.scale.y    != t.scale.y ||
                    m_transformSnapshot.scale.z    != t.scale.z;
                if (changed)
                {
                    ctx.undoSystem.PushCommand(std::make_unique<TransformCommand>(
                        &reg, ctx.selectedEntity, m_transformSnapshot, t));
                }
                m_transformEditing = false;
            }
        }

        // MeshRenderer
        if (reg.all_of<MeshRenderer>(ctx.selectedEntity))
        {
            if (IconHeader(ic, ic ? ic->entMesh : 0, "MeshRenderer"))
            {
                auto& r = reg.get<MeshRenderer>(ctx.selectedEntity);
                ImGui::Text("Meshes: %d", static_cast<int>(r.meshes.size()));
                ImGui::Text("Materials: %d", static_cast<int>(r.materials.size()));
            }
        }

        // SkeletalAnimation
        if (reg.all_of<SkeletalAnimation>(ctx.selectedEntity))
        {
            auto& skelAnim = reg.get<SkeletalAnimation>(ctx.selectedEntity);
            if (skelAnim.animator && IconHeader(ic, ic ? ic->entMesh : 0, "SkeletalAnimation"))
            {
                ImGui::Text("Bones: %d",
                    static_cast<int>(skelAnim.skeleton ? skelAnim.skeleton->GetBoneCount() : 0));
                ImGui::Text("Clips: %d", static_cast<int>(skelAnim.clips.size()));

                for (i32 i = 0; i < static_cast<i32>(skelAnim.clips.size()); ++i)
                {
                    const auto& clip = skelAnim.clips[i];
                    std::string label = clip->GetName().empty()
                        ? ("Clip " + std::to_string(i))
                        : clip->GetName();
                    if (ImGui::Selectable(label.c_str()))
                        skelAnim.animator->CrossFadeTo(clip.get(), 0.3f);
                }
            }
        }

        // NodeAnimation
        if (reg.all_of<NodeAnimationComp>(ctx.selectedEntity))
        {
            auto& nodeAnim = reg.get<NodeAnimationComp>(ctx.selectedEntity);
            if (nodeAnim.nodeAnimator && IconHeader(ic, ic ? ic->entMesh : 0, "NodeAnimation"))
            {
                ImGui::Text("Clips: %d", static_cast<int>(nodeAnim.clips.size()));
                for (i32 i = 0; i < static_cast<i32>(nodeAnim.clips.size()); ++i)
                {
                    const auto& clip = nodeAnim.clips[i];
                    std::string label = clip->GetName().empty()
                        ? ("Clip " + std::to_string(i))
                        : clip->GetName();
                    if (ImGui::Selectable(label.c_str()))
                        nodeAnim.nodeAnimator->CrossFadeTo(clip.get(), 0.3f);
                }
            }
        }

        // GridPlane
        if (reg.all_of<GridPlane>(ctx.selectedEntity))
        {
            if (IconHeader(ic, ic ? ic->entEmpty : 0, "GridPlane"))
            {
                auto& gp = reg.get<GridPlane>(ctx.selectedEntity);
                ImGui::Checkbox("Enabled", &gp.enabled);
            }
        }

        // PointLight（点光源: 全方向に減衰しながら照らす）
        if (reg.all_of<PointLight>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entLight : 0, "Point Light");
            bool removed = ComponentRemoveMenu<PointLight>(reg, ctx, ctx.selectedEntity, "PointLight");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_plEdit);
                auto& pl = reg.get<PointLight>(ctx.selectedEntity);
                bool changed = false, active = false;
                changed |= ImGui::ColorEdit3("色 Color", &pl.color.x);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("強度 Intensity", &pl.intensity, 0.0f, 20.0f, "%.2f");
                active  |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("距離 Range", &pl.range, 0.1f, 100.0f, "%.1f");
                active  |= ImGui::IsItemActive();
                ImGui::TextDisabled("位置は Transform で決まります");
                EndEdit(reg, ctx, ctx.selectedEntity, m_plEdit, changed, active, "PointLight");
            }
        }

        // DirectionalLight（平行光源: 太陽。影の向きにもなる）
        if (reg.all_of<DirectionalLight>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entLight : 0, "Directional Light");
            bool removed = ComponentRemoveMenu<DirectionalLight>(reg, ctx, ctx.selectedEntity, "DirectionalLight");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_dlEdit);
                auto& dl = reg.get<DirectionalLight>(ctx.selectedEntity);
                bool changed = false, active = false;
                changed |= ImGui::ColorEdit3("色 Color", &dl.color.x);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("強度 Intensity", &dl.intensity, 0.0f, 10.0f, "%.2f");
                active  |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("環境光 Ambient", &dl.ambient, 0.0f, 1.0f, "%.2f");
                active  |= ImGui::IsItemActive();
                changed |= DirectionEditor("dlDir", dl.direction, active);
                ImGui::TextDisabled("このライトの向きが影の向きになります");
                EndEdit(reg, ctx, ctx.selectedEntity, m_dlEdit, changed, active, "DirectionalLight");
            }
        }

        // SpotLight（スポット: 円錐状に照らす。懐中電灯/車のライト等）
        if (reg.all_of<SpotLight>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entLight : 0, "Spot Light");
            bool removed = ComponentRemoveMenu<SpotLight>(reg, ctx, ctx.selectedEntity, "SpotLight");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_slEdit);
                auto& sl = reg.get<SpotLight>(ctx.selectedEntity);
                bool changed = false, active = false;
                changed |= ImGui::ColorEdit3("色 Color", &sl.color.x);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("強度 Intensity", &sl.intensity, 0.0f, 30.0f, "%.2f");
                active  |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("距離 Range", &sl.range, 0.1f, 100.0f, "%.1f");
                active  |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("内側コーン", &sl.innerConeDeg, 1.0f, 80.0f, "%.0f°");
                active  |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("外側コーン", &sl.outerConeDeg, 1.0f, 89.0f, "%.0f°");
                active  |= ImGui::IsItemActive();
                if (sl.innerConeDeg > sl.outerConeDeg) sl.innerConeDeg = sl.outerConeDeg;
                changed |= DirectionEditor("slDir", sl.direction, active);
                ImGui::TextDisabled("位置は Transform、向きは上の方向で決まります");
                EndEdit(reg, ctx, ctx.selectedEntity, m_slEdit, changed, active, "SpotLight");
            }
        }

        // CameraComponent
        if (reg.all_of<CameraComponent>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entCamera : 0, "Camera");
            bool removed = ComponentRemoveMenu<CameraComponent>(reg, ctx, ctx.selectedEntity, "Camera");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_camEdit);
                auto& cam = reg.get<CameraComponent>(ctx.selectedEntity);
                bool changed = false, active = false;
                changed |= ImGui::DragFloat("FOV", &cam.fovDegrees, 1.0f, 1.0f, 179.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Near", &cam.nearClip, 0.01f, 0.001f, 100.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Far", &cam.farClip, 10.0f, 1.0f, 100000.0f);
                active  |= ImGui::IsItemActive();
                if (ImGui::Checkbox("Active", &cam.isActive))
                {
                    changed = true;
                    // アクティブカメラは常に1つだけ
                    if (cam.isActive)
                    {
                        for (auto [oe, oc] : reg.view<CameraComponent>().each())
                            if (oe != ctx.selectedEntity) oc.isActive = false;
                    }
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_camEdit, changed, active, "Camera");
            }
        }

        // --- Audio Source ---
        if (reg.all_of<AudioSource>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entAudio : 0, "Audio Source");
            bool removed = ComponentRemoveMenu<AudioSource>(reg, ctx, ctx.selectedEntity, "Audio Source");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_audioEdit);
                auto& as = reg.get<AudioSource>(ctx.selectedEntity);
                bool changed = false, active = false;
                char buf[256] = {};
                size_t n = as.clipPath.copy(buf, sizeof(buf) - 1);
                buf[n] = '\0';
                if (ImGui::InputText("Clip (rel)", buf, sizeof(buf)))
                { as.clipPath = buf; changed = true; }
                active |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("Volume", &as.volume, 0.0f, 1.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::Checkbox("Spatial", &as.spatial);
                ImGui::SameLine();
                changed |= ImGui::Checkbox("Loop", &as.loop);
                changed |= ImGui::Checkbox("Play On Start", &as.playOnStart);
                changed |= ImGui::DragFloat("Min Distance", &as.minDistance, 0.1f, 0.0f, 1000.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Max Distance", &as.maxDistance, 0.5f, 0.1f, 5000.0f);
                active  |= ImGui::IsItemActive();
                ImGui::TextDisabled("空間化はモノラル wav のみ");
                EndEdit(reg, ctx, ctx.selectedEntity, m_audioEdit, changed, active, "Audio Source");
            }
        }

        // --- Physics ---
        {
            bool hasRb = reg.all_of<RigidBody>(ctx.selectedEntity);
            if (ImGui::Checkbox("Physics", &hasRb))
            {
                if (hasRb)
                {
                    // Add physics: auto collider + RigidBody
                    if (reg.all_of<MeshRenderer>(ctx.selectedEntity))
                    {
                        auto* mr = &reg.get<MeshRenderer>(ctx.selectedEntity);
                        auto* tf = &reg.get<Transform>(ctx.selectedEntity);
                        std::vector<DirectX::XMFLOAT3> allPoints;
                        for (const auto* mesh : mr->meshes)
                        {
                            if (!mesh) continue;
                            for (const auto& p : mesh->GetPositions())
                                allPoints.push_back({
                                    p.x * tf->scale.x,
                                    p.y * tf->scale.y,
                                    p.z * tf->scale.z });
                        }
                        constexpr size_t kMax = 256;
                        if (allPoints.size() > kMax)
                        {
                            size_t step = allPoints.size() / kMax;
                            std::vector<DirectX::XMFLOAT3> sampled;
                            for (size_t i = 0; i < allPoints.size() && sampled.size() < kMax; i += step)
                                sampled.push_back(allPoints[i]);
                            allPoints = std::move(sampled);
                        }
                        if (!allPoints.empty())
                        {
                            ConvexHullCollider col;
                            col.points = std::move(allPoints);
                            reg.emplace_or_replace<ConvexHullCollider>(ctx.selectedEntity, col);
                            ctx.undoSystem.PushCommand(std::make_unique<AddComponentCommand<ConvexHullCollider>>(
                                &reg, ctx.selectedEntity, std::move(col), "Convex Hull Collider"));
                        }
                    }
                    reg.emplace_or_replace<RigidBody>(ctx.selectedEntity);
                    ctx.undoSystem.PushCommand(std::make_unique<AddComponentCommand<RigidBody>>(
                        &reg, ctx.selectedEntity, reg.get<RigidBody>(ctx.selectedEntity), "RigidBody"));
                }
                else
                {
                    auto pushRemove = [&]<typename T>(const char* name)
                    {
                        if (reg.all_of<T>(ctx.selectedEntity))
                        {
                            ctx.undoSystem.PushCommand(std::make_unique<RemoveComponentCommand<T>>(
                                &reg, ctx.selectedEntity, reg.get<T>(ctx.selectedEntity), name));
                            reg.remove<T>(ctx.selectedEntity);
                        }
                    };
                    pushRemove.template operator()<RigidBody>("RigidBody");
                    pushRemove.template operator()<ConvexHullCollider>("Convex Hull Collider");
                    pushRemove.template operator()<BoxCollider>("Box Collider");
                    pushRemove.template operator()<SphereCollider>("Sphere Collider");
                    pushRemove.template operator()<CapsuleCollider>("Capsule Collider");
                }
            }

            if (reg.all_of<RigidBody>(ctx.selectedEntity))
            {
                BeginEdit(reg, ctx.selectedEntity, m_rbEdit);
                auto& rb = reg.get<RigidBody>(ctx.selectedEntity);
                bool changed = false, active = false;

                const char* motionTypes[] = { "Static", "Kinematic", "Dynamic" };
                int motionIdx = static_cast<int>(rb.motionType);
                if (ImGui::Combo("Motion", &motionIdx, motionTypes, 3))
                {
                    rb.motionType = static_cast<MotionType>(motionIdx);
                    changed = true;
                }

                changed |= ImGui::DragFloat("Mass", &rb.mass, 0.5f, 0.0f, 10000.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Friction", &rb.friction, 0.01f, 0.0f, 2.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Bounce", &rb.restitution, 0.01f, 0.0f, 1.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::Checkbox("Gravity", &rb.useGravity);
                EndEdit(reg, ctx, ctx.selectedEntity, m_rbEdit, changed, active, "RigidBody");
            }
        }

        // --- Colliders ---
        if (reg.all_of<BoxCollider>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entCollider : 0, "Box Collider");
            bool removed = ComponentRemoveMenu<BoxCollider>(reg, ctx, ctx.selectedEntity, "Box Collider");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_boxColEdit);
                auto& col = reg.get<BoxCollider>(ctx.selectedEntity);
                bool changed = false, active = false;
                changed |= ImGui::DragFloat3("Half Extents", &col.halfExtents.x, 0.05f, 0.01f, 1000.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat3("Offset##Box", &col.offset.x, 0.05f);
                active  |= ImGui::IsItemActive();
                EndEdit(reg, ctx, ctx.selectedEntity, m_boxColEdit, changed, active, "Box Collider");
            }
        }

        if (reg.all_of<SphereCollider>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entCollider : 0, "Sphere Collider");
            bool removed = ComponentRemoveMenu<SphereCollider>(reg, ctx, ctx.selectedEntity, "Sphere Collider");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_sphereColEdit);
                auto& col = reg.get<SphereCollider>(ctx.selectedEntity);
                bool changed = false, active = false;
                changed |= ImGui::DragFloat("Radius##Sphere", &col.radius, 0.05f, 0.01f, 1000.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat3("Offset##Sphere", &col.offset.x, 0.05f);
                active  |= ImGui::IsItemActive();
                EndEdit(reg, ctx, ctx.selectedEntity, m_sphereColEdit, changed, active, "Sphere Collider");
            }
        }

        if (reg.all_of<CapsuleCollider>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entCollider : 0, "Capsule Collider");
            bool removed = ComponentRemoveMenu<CapsuleCollider>(reg, ctx, ctx.selectedEntity, "Capsule Collider");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_capsuleColEdit);
                auto& col = reg.get<CapsuleCollider>(ctx.selectedEntity);
                bool changed = false, active = false;
                changed |= ImGui::DragFloat("Radius##Capsule", &col.radius, 0.05f, 0.01f, 1000.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Half Height", &col.halfHeight, 0.05f, 0.01f, 1000.0f);
                active  |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat3("Offset##Capsule", &col.offset.x, 0.05f);
                active  |= ImGui::IsItemActive();
                EndEdit(reg, ctx, ctx.selectedEntity, m_capsuleColEdit, changed, active, "Capsule Collider");
            }
        }

        if (reg.all_of<ConvexHullCollider>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entCollider : 0, "Convex Hull Collider");
            bool removed = ComponentRemoveMenu<ConvexHullCollider>(reg, ctx, ctx.selectedEntity, "Convex Hull Collider");
            if (open && !removed)
            {
                const auto& col = reg.get<ConvexHullCollider>(ctx.selectedEntity);
                ImGui::Text("Points: %d", static_cast<int>(col.points.size()));
                ImGui::TextDisabled("(auto-generated from mesh)");
            }
        }

        // --- Material (PBR) ---
        if (reg.all_of<MeshRenderer>(ctx.selectedEntity))
        {
            auto& mr = reg.get<MeshRenderer>(ctx.selectedEntity);
            if (!mr.meshes.empty() && mr.meshes[0] && mr.meshes[0]->GetMaterial())
            {
                const auto* mat = mr.meshes[0]->GetMaterial();
                if (IconHeader(ic, ic ? ic->entMesh : 0, "Material"))
                {
                    if (mr.overrideMetallic < 0.0f)
                        mr.overrideMetallic = mat->defaultMetallic;
                    if (mr.overrideRoughness < 0.0f)
                        mr.overrideRoughness = mat->defaultRoughness;

                    // PBR 編集前スナップショット
                    if (!m_pbrEditing)
                    {
                        m_pbrMetallicSnapshot = mr.overrideMetallic;
                        m_pbrRoughnessSnapshot = mr.overrideRoughness;
                    }

                    ImGui::SliderFloat("Metallic", &mr.overrideMetallic, 0.0f, 1.0f);
                    bool metalActive = ImGui::IsItemActive();
                    ImGui::SliderFloat("Roughness", &mr.overrideRoughness, 0.0f, 1.0f);
                    bool roughActive = ImGui::IsItemActive();

                    if (metalActive || roughActive)
                        m_pbrEditing = true;

                    if (m_pbrEditing && !metalActive && !roughActive && !ImGui::IsAnyItemActive())
                    {
                        bool changed = m_pbrMetallicSnapshot != mr.overrideMetallic
                                    || m_pbrRoughnessSnapshot != mr.overrideRoughness;
                        if (changed)
                        {
                            ctx.undoSystem.PushCommand(std::make_unique<PBRCommand>(
                                &reg, ctx.selectedEntity,
                                m_pbrMetallicSnapshot, m_pbrRoughnessSnapshot,
                                mr.overrideMetallic, mr.overrideRoughness));
                        }
                        m_pbrEditing = false;
                    }

                    bool hasNormal = mat->normalMapTexture != nullptr;
                    bool hasMR2 = mat->metalRoughnessTexture != nullptr;
                    ImGui::Text("Normal Map: %s", hasNormal ? "Yes" : "No");
                    ImGui::Text("MetalRough Map: %s", hasMR2 ? "Yes" : "No");
                }
            }

            // UV タイリング
            if (ImGui::CollapsingHeader("UV Tiling"))
            {
                bool uvChanged = false;
                uvChanged |= ImGui::DragFloat("U Scale", &mr.uvScaleU, 0.1f, 0.01f, 100.0f);
                uvChanged |= ImGui::DragFloat("V Scale", &mr.uvScaleV, 0.1f, 0.01f, 100.0f);
                // U,V を連動させるボタン
                if (ImGui::Button("U=V"))
                {
                    mr.uvScaleV = mr.uvScaleU;
                    uvChanged = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset 1x"))
                {
                    mr.uvScaleU = 1.0f;
                    mr.uvScaleV = 1.0f;
                    uvChanged = true;
                }
                if (uvChanged)
                {
                    // 全メッシュに UV スケールを適用
                    for (auto* mesh : mr.meshes)
                    {
                        if (mesh)
                            mesh->ApplyUVScale(*scene->GetDevice(), mr.uvScaleU, mr.uvScaleV);
                    }
                }
            }
        }

        // LuaScript
        DrawLuaScriptSection(reg, ctx.selectedEntity, m_scriptEngine, m_assetsDir);

        // --- Add Component ---
        ImGui::Separator();
        // ✚ コンポーネント追加
        if (ImGui::Button("\xe2\x9c\x9a \xe3\x82\xb3\xe3\x83\xb3\xe3\x83\x9d\xe3\x83\xbc\xe3\x83\x8d\xe3\x83\xb3\xe3\x83\x88\xe8\xbf\xbd\xe5\x8a\xa0", ImVec2(-1, 0)))
            ImGui::OpenPopup("AddComponentPopup");

        if (ImGui::BeginPopup("AddComponentPopup"))
        {
            AddComponentMenuItem<PointLight>(reg, ctx, ctx.selectedEntity, "Point Light");
            AddComponentMenuItem<DirectionalLight>(reg, ctx, ctx.selectedEntity, "Directional Light");
            AddComponentMenuItem<SpotLight>(reg, ctx, ctx.selectedEntity, "Spot Light");
            AddComponentMenuItem<CameraComponent>(reg, ctx, ctx.selectedEntity, "Camera");
            AddComponentMenuItem<AudioSource>(reg, ctx, ctx.selectedEntity, "Audio Source");
            ImGui::Separator();
            AddComponentMenuItem<RigidBody>(reg, ctx, ctx.selectedEntity, "RigidBody");
            AddComponentMenuItem<BoxCollider>(reg, ctx, ctx.selectedEntity, "Box Collider");
            AddComponentMenuItem<SphereCollider>(reg, ctx, ctx.selectedEntity, "Sphere Collider");
            AddComponentMenuItem<CapsuleCollider>(reg, ctx, ctx.selectedEntity, "Capsule Collider");
            ImGui::EndPopup();
        }
    }
    else
    {
        ImGui::TextDisabled("\xe3\x82\xa8\xe3\x83\xb3\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x86\xe3\x82\xa3\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84");  // Select an entity
    }

    ImGui::Separator();

    // --- Camera ---
    if (IconHeader(ctx.icons, ctx.icons ? ctx.icons->entCamera : 0,
                   "\xe3\x82\xab\xe3\x83\xa1\xe3\x83\xa9", ImGuiTreeNodeFlags_DefaultOpen))  // Camera
    {
        auto camPos = camera->GetPosition();
        ImGui::Text("%.1f, %.1f, %.1f", camPos.x, camPos.y, camPos.z);
        f32 moveSpeed = camera->GetMoveSpeed();
        if (ImGui::SliderFloat("\xe9\x80\x9f\xe5\xba\xa6", &moveSpeed, 1.0f, 50.0f))  // Speed
            camera->SetMoveSpeed(moveSpeed);
    }

    // --- Shadow quality ---
    if (ImGui::CollapsingHeader("\xe3\x82\xb7\xe3\x83\xa3\xe3\x83\x89\xe3\x82\xa6"))  // Shadow
    {
        const char* qualities[] = {"1024 (Low)", "2048 (Medium)", "4096 (High)", "8192 (Ultra)"};
        const u32 sizes[] = {1024, 2048, 4096, 8192};
        if (ImGui::Combo("\xe8\xa7\xa3\xe5\x83\x8f\xe5\xba\xa6", &shadowQualityIndex, qualities, 4))  // Resolution
        {
            shadowMapSize = sizes[shadowQualityIndex];
            shadowMapDirty = true;
        }
        ImGui::Text("%ux%u", shadowMapSize, shadowMapSize);
    }

    // --- Audio ---
    if (IconHeader(ctx.icons, ctx.icons ? ctx.icons->entAudio : 0,
                   "\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xaa"))  // Audio
    {
        f32 masterVol = audioSystem->GetMasterVolume();
        f32 bgmVol    = audioSystem->GetBGMVolume();
        f32 sfxVol    = audioSystem->GetSFXVolume();
        if (ImGui::SliderFloat("\xe3\x83\x9e\xe3\x82\xb9\xe3\x82\xbf\xe3\x83\xbc", &masterVol, 0.0f, 1.0f))
            audioSystem->SetMasterVolume(masterVol);
        if (ImGui::SliderFloat("BGM", &bgmVol, 0.0f, 1.0f))
            audioSystem->SetBGMVolume(bgmVol);
        if (ImGui::SliderFloat("SE", &sfxVol, 0.0f, 1.0f))
            audioSystem->SetSFXVolume(sfxVol);

        const auto& bgmList = audioSystem->GetBGMList();
        for (const auto& bgm : bgmList)
        {
            std::string fn = std::filesystem::path(bgm).filename().string();
            ImGui::PushID(bgm.c_str());
            if (ImGui::Button("\xe2\x96\xb6")) audioSystem->PlayBGM(bgm);
            ImGui::SameLine(); ImGui::Text("%s", fn.c_str());
            ImGui::PopID();
        }
        if (!bgmList.empty() && ImGui::Button("BGM \xe5\x81\x9c\xe6\xad\xa2"))
            audioSystem->StopBGM();

        const auto& sfxList = audioSystem->GetSFXList();
        for (const auto& sfx : sfxList)
        {
            std::string fn = std::filesystem::path(sfx).filename().string();
            ImGui::PushID(sfx.c_str());
            if (ImGui::Button("\xe2\x96\xb6")) audioSystem->PlaySFX(sfx);
            ImGui::SameLine(); ImGui::Text("%s", fn.c_str());
            ImGui::PopID();
        }
        if (!sfxList.empty() && ImGui::Button("SE \xe5\x85\xa8\xe5\x81\x9c\xe6\xad\xa2"))
            audioSystem->StopAllSFX();
    }

    // --- Settings ---
    if (ImGui::CollapsingHeader("\xe8\xa8\xad\xe5\xae\x9a"))  // Settings
    {
        ImGui::Checkbox("VSync", &useVsync);

        bool debugDraw = physicsDebugRenderer->IsEnabled();
        if (ImGui::Checkbox("Physics Debug", &debugDraw))
        {
            physicsDebugRenderer->SetEnabled(debugDraw);
            physicsDebugDraw = debugDraw;
        }
    }

    // --- Build ---
    if (IconHeader(ctx.icons, ctx.icons ? ctx.icons->build : 0,
                   "\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89"))  // Build
    {
        if (ImGui::Button("\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89"))  // Game Build
        {
            ctx.pendingBuildGame = true;
        }
        if (ctx.buildCompleteFlash > 0.0f)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.5f, 1.0f));
            ImGui::Text("\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89\xe5\xae\x8c\xe4\xba\x86!");  // Build complete!
            ImGui::PopStyleColor();
            ctx.buildCompleteFlash -= clock->GetDeltaTime();
        }
    }

    // Inspector 全体を DND_SCRIPT ドロップターゲットに
    if (ctx.HasSelection() && ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_SCRIPT"))
        {
            const char* pathCStr = static_cast<const char*>(payload->Data);
            std::string absPath(pathCStr);

            namespace fs = std::filesystem;
            auto abs  = fs::path(absPath).lexically_normal().string();
            auto base = fs::path(m_assetsDir).lexically_normal().string();
            std::replace(abs.begin(),  abs.end(),  '\\', '/');
            std::replace(base.begin(), base.end(), '\\', '/');
            std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;

            for (auto ent : ctx.selectedEntities)
                ctx.pendingScriptAttachments.push_back({ent, rel});
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::End();
}

} // namespace dx12e
