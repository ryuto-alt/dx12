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

#include <imgui_internal.h>   // BeginDragDropTargetCustom（ウィンドウ全体をドロップ先に）
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
                   CapsuleCollider, ConvexHullCollider, CharacterController>(e))   return ic.entPhysics;
    if (reg.all_of<LuaScript>(e))                             return ic.entScript;
    return ic.entEmpty;
}

// assets 配下の .lua（= スクリプトコンポーネント候補）を列挙する。
// game.lua は除外。戻り値は assets 相対パス（スラッシュ区切り、例 "components/Spike.lua"）。
// メニュー/ポップアップが開いている間だけ呼ばれる想定（開いてる時しか走らないので軽い）。
std::vector<std::string> ScanScriptComponents(const std::string& assetsDir)
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    fs::path base(assetsDir);
    if (base.empty() || !fs::exists(base, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(base, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec) break;
        const auto& p = it->path();
        if (!fs::is_regular_file(p, ec)) continue;
        if (p.extension() != ".lua") continue;
        if (p.filename() == "game.lua") continue;
        std::string rel = fs::relative(p, base, ec).generic_string();
        if (!rel.empty()) out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// スクリプト選択 UI。assets の .lua を列挙し、クリックで pendingScriptAttachments に積む
// （フレーム境界で Undo 付きアタッチ）。選んだら true。
bool ScriptPicker(dx12e::EditorContext& ctx, entt::entity e, const std::string& assetsDir)
{
    bool picked = false;
    auto scripts = ScanScriptComponents(assetsDir);
    if (scripts.empty())
    {
        ImGui::TextDisabled("assets/components に .lua がありません");
        ImGui::TextDisabled("（AssetBrowser の「新規スクリプト」で作成）");
        return false;
    }
    for (const auto& rel : scripts)
    {
        if (ImGui::Selectable(rel.c_str()))
        {
            ctx.pendingScriptAttachments.push_back({ e, rel });
            picked = true;
        }
    }
    return picked;
}

// スキーマに合わせて ls.props を並べ替え/補完する（既存値は名前で引き継ぎ、
// 宣言から消えたプロパティは捨てる）。これで Inspector は常にスキーマ通りに描ける。
void SyncScriptProps(dx12e::LuaScript& ls, const std::vector<dx12e::ScriptPropDef>& schema)
{
    std::vector<dx12e::ScriptProp> next;
    next.reserve(schema.size());
    for (const auto& d : schema)
    {
        dx12e::ScriptProp p;
        p.name = d.name;
        p.type = d.type;
        const dx12e::ScriptProp* old = nullptr;
        for (auto& ex : ls.props)
            if (ex.name == d.name && ex.type == d.type) { old = &ex; break; }
        if (old) { p.num = old->num; p.b = old->b; p.str = old->str; p.vec = old->vec; }
        else     { p.num = d.def.num; p.b = d.def.b; p.str = d.def.str; p.vec = d.def.vec; }
        next.push_back(std::move(p));
    }
    ls.props = std::move(next);
}

void DrawLuaScriptSection(entt::registry& reg,
                          entt::entity e,
                          dx12e::EditorContext& ctx,
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
        if (ImGui::Button("＋ スクリプトを付ける", ImVec2(-1, 0)))
            ImGui::OpenPopup("PickScriptPopup");
        ImGui::TextDisabled("または AssetBrowser から .lua をドラッグ");
        if (ImGui::BeginPopup("PickScriptPopup"))
        {
            ImGui::TextDisabled("付けるスクリプトを選ぶ:");
            ImGui::Separator();
            if (ScriptPicker(ctx, e, assetsDir)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
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
    if (ImGui::Button("\xe5\xa4\x89\xe6\x9b\xb4##LuaScript"))  // 変更（別スクリプトへ差し替え）
        ImGui::OpenPopup("ChangeScriptPopup");
    if (ImGui::BeginPopup("ChangeScriptPopup"))
    {
        ImGui::TextDisabled("\xe5\x88\xa5\xe3\x81\xae\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88\xe3\x81\xb8\xe5\xa4\x89\xe6\x9b\xb4:");  // 別のスクリプトへ変更:
        ImGui::Separator();
        if (ScriptPicker(ctx, e, assetsDir)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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

    // --- 公開プロパティ（.lua の properties 宣言からスキーマ駆動で自動生成）---
    // ここを編集すると各エンティティ個別の値としてシーンに保存され、Play 時に
    // self.<name> としてスクリプトへ注入される（巨大コントローラ不要の部品化）。
    if (scriptEngine)
    {
        const auto& schema = scriptEngine->GetPropertySchema(ls.scriptPath);
        if (!schema.empty())
        {
            SyncScriptProps(ls, schema);
            ImGui::SeparatorText("プロパティ");
            ImGui::PushItemWidth(-140.0f);
            for (size_t i = 0; i < schema.size(); ++i)
            {
                const auto& d = schema[i];
                auto& p = ls.props[i];
                const char* lbl = d.label.empty() ? d.name.c_str() : d.label.c_str();
                ImGui::PushID(static_cast<int>(i));
                switch (d.type)
                {
                case dx12e::ScriptPropType::Float:
                {
                    float v = static_cast<float>(p.num);
                    bool ch = d.hasRange ? ImGui::SliderFloat(lbl, &v, d.minVal, d.maxVal)
                                         : ImGui::DragFloat(lbl, &v, 0.01f);
                    if (ch) p.num = v;
                    break;
                }
                case dx12e::ScriptPropType::Int:
                {
                    int v = static_cast<int>(p.num);
                    bool ch = d.hasRange ? ImGui::SliderInt(lbl, &v, static_cast<int>(d.minVal), static_cast<int>(d.maxVal))
                                         : ImGui::DragInt(lbl, &v);
                    if (ch) p.num = v;
                    break;
                }
                case dx12e::ScriptPropType::Bool:
                    ImGui::Checkbox(lbl, &p.b);
                    break;
                case dx12e::ScriptPropType::String:
                {
                    char buf[256];
                    std::memset(buf, 0, sizeof(buf));
                    strncpy_s(buf, sizeof(buf), p.str.c_str(), _TRUNCATE);
                    if (ImGui::InputText(lbl, buf, sizeof(buf)))
                        p.str = buf;
                    break;
                }
                case dx12e::ScriptPropType::Vec3:
                    ImGui::DragFloat3(lbl, &p.vec.x, 0.01f);
                    break;
                case dx12e::ScriptPropType::Color:
                    ImGui::ColorEdit3(lbl, &p.vec.x);
                    break;
                case dx12e::ScriptPropType::Entity:
                {
                    // シーン内のエンティティ名から選ぶコンボ（参照先を名前で保持）。
                    const char* cur = p.str.empty() ? "(\xe3\x81\xaa\xe3\x81\x97)" : p.str.c_str();  // (なし)
                    if (ImGui::BeginCombo(lbl, cur))
                    {
                        if (ImGui::Selectable("(\xe3\x81\xaa\xe3\x81\x97)", p.str.empty()))  // (なし)
                            p.str.clear();
                        auto nameView = reg.view<dx12e::NameTag>();
                        for (auto ne : nameView)
                        {
                            const auto& nm = nameView.get<dx12e::NameTag>(ne).name;
                            if (nm.empty()) continue;
                            if (ImGui::Selectable(nm.c_str(), nm == p.str))
                                p.str = nm;
                        }
                        ImGui::EndCombo();
                    }
                    break;
                }
                }
                ImGui::PopID();
            }
            ImGui::PopItemWidth();
            if (ImGui::SmallButton("規定値に戻す"))
            {
                for (size_t i = 0; i < schema.size(); ++i)
                {
                    auto& p = ls.props[i]; const auto& d = schema[i];
                    p.num = d.def.num; p.b = d.def.b; p.str = d.def.str; p.vec = d.def.vec;
                }
            }
        }
        else
        {
            ImGui::TextDisabled("properties = {...} を書くとここに編集欄が出ます");
        }
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

        // 種別専用インスペクター（ライト/オーディオは専用UIを最前面に。共通部品はこの下）
        if (reg.any_of<PointLight, DirectionalLight, SpotLight>(ctx.selectedEntity))
            RenderLightHero(reg, ctx, ctx.selectedEntity);
        if (reg.all_of<AudioSource>(ctx.selectedEntity))
            RenderAudioHero(reg, ctx, ctx.selectedEntity);

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

        // ライト（Point / Directional / Spot）は上部の専用ヒーローカード（RenderLightHero）で編集する。

        // Gimmick（ステージギミック: 時間で動く/塞ぐ部品。動きは Lua が駆動）
        if (reg.all_of<Gimmick>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "Gimmick");
            bool removed = ComponentRemoveMenu<Gimmick>(reg, ctx, ctx.selectedEntity, "Gimmick");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_gimmickEdit);
                auto& gm = reg.get<Gimmick>(ctx.selectedEntity);
                bool changed = false, active = false;
                const char* kinds[] = { "Static Wall（動かない壁）", "Spike Pulse（上下するトゲ）",
                                        "Slide X（左右に動く）", "Slide Z（前後に動く）" };
                changed |= ImGui::Combo("種類 Kind", &gm.kind, kinds, IM_ARRAYSIZE(kinds));
                if (gm.kind < 0) gm.kind = 0; if (gm.kind > 3) gm.kind = 3;
                if (gm.kind != 0)  // Static 以外は動きパラメータを表示
                {
                    changed |= ImGui::DragFloat("周期 Period(s)", &gm.period, 0.05f, 0.2f, 30.0f, "%.2f");
                    active  |= ImGui::IsItemActive();
                    changed |= ImGui::SliderFloat("位相 Phase", &gm.phase, 0.0f, 1.0f, "%.2f");
                    active  |= ImGui::IsItemActive();
                    changed |= ImGui::DragFloat("振幅 Amplitude", &gm.amplitude, 0.05f, 0.0f, 30.0f, "%.2f");
                    active  |= ImGui::IsItemActive();
                }
                if (gm.kind == 1)  // SpikePulse 固有
                {
                    changed |= ImGui::SliderFloat("塞ぐ閾値 Threshold", &gm.threshold, 0.0f, 1.0f, "%.2f");
                    active  |= ImGui::IsItemActive();
                    changed |= ImGui::Checkbox("直撃死 Deadly", &gm.deadly);
                }
                changed |= ImGui::Checkbox("当たり判定 Solid", &gm.solid);
                ImGui::TextDisabled("基準位置=Transform。動き/早送りはゲーム側が駆動します");
                EndEdit(reg, ctx, ctx.selectedEntity, m_gimmickEdit, changed, active, "Gimmick");
            }
        }

        // ParticleEmitter（配置できるエフェクト部品）
        if (reg.all_of<ParticleEmitter>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "Particle Emitter");
            bool removed = ComponentRemoveMenu<ParticleEmitter>(reg, ctx, ctx.selectedEntity, "Particle Emitter");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_emitterEdit);
                auto& pe = reg.get<ParticleEmitter>(ctx.selectedEntity);
                bool changed = false, active = false;
                const char* kinds[] = { "Glow", "Fire", "Smoke", "Spark", "Magic", "Electric", "Ring", "Star" };
                changed |= ImGui::Combo("見た目 Kind", &pe.kind, kinds, IM_ARRAYSIZE(kinds));
                const char* blends[] = { "加算 Additive", "アルファ Alpha" };
                changed |= ImGui::Combo("合成 Blend", &pe.blend, blends, IM_ARRAYSIZE(blends));
                changed |= ImGui::DragFloat("放出レート Rate(/s)", &pe.rate, 0.5f, 0.0f, 500.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::Checkbox("Play開始で放出 playOnStart", &pe.playOnStart);
                changed |= ImGui::Checkbox("ループ Looping", &pe.looping);
                if (!pe.looping)
                { changed |= ImGui::DragFloat("継続秒 Duration", &pe.duration, 0.05f, 0.0f, 60.0f); active |= ImGui::IsItemActive(); }
                ImGui::SeparatorText("見た目");
                changed |= ImGui::ColorEdit3("開始色 Color", &pe.color.x);
                changed |= ImGui::ColorEdit3("終了色 ColorEnd", &pe.colorEnd.x);
                changed |= ImGui::DragFloat("輝度 Intensity", &pe.intensity, 0.05f, 0.0f, 30.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("サイズ Size", &pe.size, 0.01f, 0.0f, 10.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("終了サイズ SizeEnd", &pe.sizeEnd, 0.01f, 0.0f, 10.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("寿命 Life(s)", &pe.life, 0.01f, 0.01f, 30.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("寿命ばらつき LifeVar", &pe.lifeVar, 0.0f, 1.0f); active |= ImGui::IsItemActive();
                ImGui::SeparatorText("動き");
                changed |= ImGui::DragFloat3("方向 Dir", &pe.dir.x, 0.01f); active |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("拡がり Spread", &pe.spread, 0.0f, 1.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("速度 Speed", &pe.speed, 0.02f, 0.0f, 50.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::SliderFloat("速度ばらつき SpeedVar", &pe.speedVar, 0.0f, 1.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("重力 Gravity", &pe.gravity, 0.02f, -50.0f, 50.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("抵抗 Drag", &pe.drag, 0.02f, 0.0f, 10.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("上向き Up", &pe.up, 0.02f, 0.0f, 10.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("ストレッチ Stretch", &pe.stretch, 0.02f, 0.0f, 10.0f); active |= ImGui::IsItemActive();
                ImGui::TextDisabled("エディタでもプレビュー表示されます");
                EndEdit(reg, ctx, ctx.selectedEntity, m_emitterEdit, changed, active, "Particle Emitter");
            }
        }

        // Trigger（イベント: 範囲に入る/出る/居る で宣言アクションを実行）
        if (reg.all_of<Trigger>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "Trigger");
            bool removed = ComponentRemoveMenu<Trigger>(reg, ctx, ctx.selectedEntity, "Trigger");
            if (open && !removed)
            {
                auto& tr = reg.get<Trigger>(ctx.selectedEntity);
                const char* shapes[] = { "Box", "Sphere" };
                ImGui::Combo("形 Shape", &tr.shape, shapes, IM_ARRAYSIZE(shapes));
                if (tr.shape == 0) ImGui::DragFloat3("半径 HalfExtents", &tr.halfExtents.x, 0.05f, 0.0f, 1000.0f);
                else               ImGui::DragFloat("半径 Radius", &tr.radius, 0.05f, 0.0f, 1000.0f);
                ImGui::DragFloat3("オフセット Offset", &tr.offset.x, 0.05f);
                {
                    const char* cur = tr.filter.empty() ? "Player（既定）" : tr.filter.c_str();
                    if (ImGui::BeginCombo("対象 Filter", cur))
                    {
                        if (ImGui::Selectable("Player（既定）", tr.filter.empty())) tr.filter.clear();
                        auto nv = reg.view<dx12e::NameTag>();
                        for (auto ne : nv)
                        { const auto& nm = nv.get<dx12e::NameTag>(ne).name; if (nm.empty()) continue;
                          if (ImGui::Selectable(nm.c_str(), nm == tr.filter)) tr.filter = nm; }
                        ImGui::EndCombo();
                    }
                }
                ImGui::Checkbox("一度だけ Once", &tr.once);
                ImGui::SeparatorText("アクション");
                int removeIdx = -1;
                for (size_t i = 0; i < tr.actions.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    auto& a = tr.actions[i];
                    const char* whens[] = { "入った時 Enter", "出た時 Exit", "居る間 Stay" };
                    ImGui::Combo("いつ When", &a.when, whens, IM_ARRAYSIZE(whens));
                    const char* types[] = { "Enable", "Disable", "Destroy", "Move", "PlayEffect",
                                            "StopEffect", "PlaySound", "LoadScene", "FadeToScene",
                                            "SetProperty", "EmitEvent" };
                    ImGui::Combo("何を Type", &a.type, types, IM_ARRAYSIZE(types));
                    {
                        const char* cur = a.target.empty() ? "(なし=Filter対象)" : a.target.c_str();
                        if (ImGui::BeginCombo("対象 Target", cur))
                        {
                            if (ImGui::Selectable("(なし=Filter対象)", a.target.empty())) a.target.clear();
                            auto nv = reg.view<dx12e::NameTag>();
                            for (auto ne : nv)
                            { const auto& nm = nv.get<dx12e::NameTag>(ne).name; if (nm.empty()) continue;
                              if (ImGui::Selectable(nm.c_str(), nm == a.target)) a.target = nm; }
                            ImGui::EndCombo();
                        }
                    }
                    char buf[256];
                    if (a.type == 3) ImGui::DragFloat3("移動量 Vec", &a.vec.x, 0.05f);
                    if (a.type == 7 || a.type == 8)
                    { std::memset(buf, 0, sizeof(buf)); strncpy_s(buf, sizeof(buf), a.str.c_str(), _TRUNCATE);
                      if (ImGui::InputText("シーン Path", buf, sizeof(buf))) a.str = buf; }
                    if (a.type == 8)
                    { float d = static_cast<float>(a.num); if (ImGui::DragFloat("秒 Dur", &d, 0.05f, 0.0f, 10.0f)) a.num = d; }
                    if (a.type == 9 || a.type == 10)
                    { std::memset(buf, 0, sizeof(buf)); strncpy_s(buf, sizeof(buf), a.str.c_str(), _TRUNCATE);
                      const char* hint = a.type == 9 ? "プロパティ名 Prop" : "イベント名 Event";
                      if (ImGui::InputText(hint, buf, sizeof(buf))) a.str = buf;
                      float v = static_cast<float>(a.num); if (ImGui::DragFloat("値 Value", &v, 0.05f)) a.num = v; }
                    if (ImGui::SmallButton("このアクションを削除")) removeIdx = static_cast<int>(i);
                    ImGui::Separator();
                    ImGui::PopID();
                }
                if (removeIdx >= 0) tr.actions.erase(tr.actions.begin() + removeIdx);
                if (ImGui::Button("＋ アクション追加")) tr.actions.push_back(dx12e::TriggerAction{});
                ImGui::TextDisabled("Play 中に評価。Target 空のアクションは Filter 対象に作用");
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

                // 投影方式（透視 / 正射）。正射＝平行投影は距離で大きさが変わらない（2D/見下ろし向け）。
                // 近づくとスプライト/メッシュを大きく見せたいなら「透視」を選ぶ。
                int projIdx = (cam.projection == CameraProjection::Orthographic) ? 1 : 0;
                const char* projItems[] = { "Perspective (透視)", "Orthographic (正射)" };
                if (ImGui::Combo("Projection", &projIdx, projItems, 2))
                {
                    cam.projection = (projIdx == 1) ? CameraProjection::Orthographic
                                                    : CameraProjection::Perspective;
                    changed = true;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("正射は距離で大きさが変わりません（2D/見下ろし向け）。\n"
                                      "近づくと大きくしたいなら透視を選びます。");

                // 透視は FOV、正射は Ortho Size（ビュー縦の半分の世界単位）を出す。
                if (cam.projection == CameraProjection::Perspective)
                {
                    changed |= ImGui::DragFloat("FOV", &cam.fovDegrees, 1.0f, 1.0f, 179.0f);
                    active  |= ImGui::IsItemActive();
                }
                else
                {
                    changed |= ImGui::DragFloat("Ortho Size", &cam.orthoSize, 0.1f, 0.01f, 1000.0f);
                    active  |= ImGui::IsItemActive();
                }
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

        // オーディオ（AudioSource）は上部の専用ヒーローカード（RenderAudioHero）で編集する。

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

        if (reg.all_of<CharacterController>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entPhysics : 0, "Character Controller");
            bool removed = ComponentRemoveMenu<CharacterController>(reg, ctx, ctx.selectedEntity, "Character Controller");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_ccEdit);
                auto& cc = reg.get<CharacterController>(ctx.selectedEntity);
                bool changed = false, active = false;
                changed |= ImGui::DragFloat("Radius##CC",     &cc.radius,      0.02f, 0.05f, 5.0f);   active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Half Height##CC",&cc.halfHeight,  0.02f, 0.05f, 5.0f);   active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat3("Offset##CC",    &cc.offset.x,    0.02f);                active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Mass##CC",       &cc.mass,        1.0f,  1.0f, 1000.0f); active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Max Slope(deg)", &cc.maxSlopeDeg, 0.5f,  0.0f, 89.0f);   active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Step Height",    &cc.stepHeight,  0.01f, 0.0f, 2.0f);    active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Jump Speed",     &cc.jumpSpeed,   0.1f,  0.0f, 50.0f);   active |= ImGui::IsItemActive();
                changed |= ImGui::DragFloat("Gravity Scale",  &cc.gravityScale,0.05f, 0.0f, 5.0f);    active |= ImGui::IsItemActive();
                EndEdit(reg, ctx, ctx.selectedEntity, m_ccEdit, changed, active, "Character Controller");
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
        DrawLuaScriptSection(reg, ctx.selectedEntity, ctx, m_scriptEngine, m_assetsDir);

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
            AddComponentMenuItem<Gimmick>(reg, ctx, ctx.selectedEntity, "Gimmick");
            AddComponentMenuItem<ParticleEmitter>(reg, ctx, ctx.selectedEntity, "Particle Emitter");
            AddComponentMenuItem<Trigger>(reg, ctx, ctx.selectedEntity, "Trigger");
            ImGui::Separator();
            AddComponentMenuItem<RigidBody>(reg, ctx, ctx.selectedEntity, "RigidBody");
            AddComponentMenuItem<BoxCollider>(reg, ctx, ctx.selectedEntity, "Box Collider");
            AddComponentMenuItem<SphereCollider>(reg, ctx, ctx.selectedEntity, "Sphere Collider");
            AddComponentMenuItem<CapsuleCollider>(reg, ctx, ctx.selectedEntity, "Capsule Collider");
            AddComponentMenuItem<CharacterController>(reg, ctx, ctx.selectedEntity, "Character Controller");
            ImGui::Separator();
            // スクリプト（.lua）をクリックでアタッチ — ドラッグ不要
            if (ImGui::BeginMenu("\xe3\x82\xb9\xe3\x82\xaf\xe3\x83\xaa\xe3\x83\x97\xe3\x83\x88"))  // スクリプト
            {
                ScriptPicker(ctx, ctx.selectedEntity, m_assetsDir);
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
    }
    else
    {
        ImGui::TextDisabled("\xe3\x82\xa8\xe3\x83\xb3\xe3\x83\x86\xe3\x82\xa3\xe3\x83\x86\xe3\x82\xa3\xe3\x82\x92\xe9\x81\xb8\xe6\x8a\x9e\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84");  // Select an entity
    }

    // Inspector ウィンドウ全体を .lua のドロップ先にする（どこにドロップしても付く）
    if (ctx.HasSelection())
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (win && ImGui::BeginDragDropTargetCustom(win->Rect(), win->ID))
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
    }

    ImGui::End();
}

// グローバルなエンジン設定（カメラ速度/シャドウ/オーディオ/VSync/ビルド）。
// Inspector から分離して独立ウィンドウ「エンジン設定」に描く（下ドックに置く）。
// ── ライト専用インスペクター（明るさ/色/距離/コーン/方向）。種別を判定して該当UIを出す ──
void InspectorPanel::RenderLightHero(entt::registry& reg, EditorContext& ctx, entt::entity e)
{
    const EditorUiIcons* ic = ctx.icons;
    const ImVec4 amber(0.96f, 0.72f, 0.25f, 1.0f);

    // ヘッダ（アイコン + 「ライト」 + 種別名）
    if (ic && ic->entLight)
    {
        float s = ImGui::GetTextLineHeight() * 1.3f;
        ImGui::Image(static_cast<ImTextureID>(ic->entLight), ImVec2(s, s));
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::AlignTextToFramePadding();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, amber);
    ImGui::TextUnformatted("ライト");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextDisabled("%s",
        reg.all_of<DirectionalLight>(e) ? "Directional — 太陽光（全体を照らす）" :
        reg.all_of<SpotLight>(e)        ? "Spot — スポット（円錐状）"          :
                                          "Point — 点光源（全方向）");

    ImGui::Spacing();
    ImGui::PushItemWidth(-110.0f);

    // 色 × 明るさ の結果を帯でプレビュー（実際の光の見え方の目安）
    auto previewSwatch = [&](const DirectX::XMFLOAT3& col, float intensity)
    {
        float k = intensity > 1.0f ? 1.0f : intensity;
        ImVec4 c(col.x * k, col.y * k, col.z * k, 1.0f);
        ImGui::ColorButton("##lightpreview", c,
            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
            ImVec2(ImGui::GetContentRegionAvail().x, 14.0f));
        ImGui::Spacing();
    };

    if (reg.all_of<PointLight>(e))
    {
        BeginEdit(reg, e, m_plEdit);
        auto& pl = reg.get<PointLight>(e);
        bool changed = false, active = false;
        previewSwatch(pl.color, pl.intensity);
        changed |= ImGui::ColorEdit3("色 Color", &pl.color.x, ImGuiColorEditFlags_NoInputs);
        active  |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("明るさ Brightness", &pl.intensity, 0.0f, 20.0f, "%.2f");
        active  |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("距離 Range", &pl.range, 0.1f, 100.0f, "%.1f");
        active  |= ImGui::IsItemActive();
        ImGui::TextDisabled("位置は Transform で決まります");
        EndEdit(reg, ctx, e, m_plEdit, changed, active, "PointLight");
    }
    if (reg.all_of<DirectionalLight>(e))
    {
        BeginEdit(reg, e, m_dlEdit);
        auto& dl = reg.get<DirectionalLight>(e);
        bool changed = false, active = false;
        previewSwatch(dl.color, dl.intensity);
        changed |= ImGui::ColorEdit3("色 Color", &dl.color.x, ImGuiColorEditFlags_NoInputs);
        active  |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("明るさ Brightness", &dl.intensity, 0.0f, 10.0f, "%.2f");
        active  |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("環境光 Ambient", &dl.ambient, 0.0f, 1.0f, "%.2f");
        active  |= ImGui::IsItemActive();
        changed |= DirectionEditor("dlDir", dl.direction, active);
        ImGui::TextDisabled("このライトの向きが影の向きになります");
        EndEdit(reg, ctx, e, m_dlEdit, changed, active, "DirectionalLight");
    }
    if (reg.all_of<SpotLight>(e))
    {
        BeginEdit(reg, e, m_slEdit);
        auto& sl = reg.get<SpotLight>(e);
        bool changed = false, active = false;
        previewSwatch(sl.color, sl.intensity);
        changed |= ImGui::ColorEdit3("色 Color", &sl.color.x, ImGuiColorEditFlags_NoInputs);
        active  |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("明るさ Brightness", &sl.intensity, 0.0f, 30.0f, "%.2f");
        active  |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("距離 Range", &sl.range, 0.1f, 100.0f, "%.1f");
        active  |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("内側コーン Inner", &sl.innerConeDeg, 1.0f, 80.0f, "%.0f°");
        active  |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("外側コーン Outer", &sl.outerConeDeg, 1.0f, 89.0f, "%.0f°");
        active  |= ImGui::IsItemActive();
        if (sl.innerConeDeg > sl.outerConeDeg) sl.innerConeDeg = sl.outerConeDeg;
        changed |= DirectionEditor("slDir", sl.direction, active);
        ImGui::TextDisabled("位置は Transform、向きは上の方向で決まります");
        EndEdit(reg, ctx, e, m_slEdit, changed, active, "SpotLight");
    }

    ImGui::PopItemWidth();
    ImGui::Separator();
}

// ── オーディオ専用インスペクター（クリップ/音量/再生/空間化） ──
void InspectorPanel::RenderAudioHero(entt::registry& reg, EditorContext& ctx, entt::entity e)
{
    const EditorUiIcons* ic = ctx.icons;
    const ImVec4 green(0.37f, 0.78f, 0.49f, 1.0f);

    auto& as = reg.get<AudioSource>(e);

    // ヘッダ（アイコン + 「オーディオ」 + 2D/3D 種別）
    if (ic && ic->entAudio)
    {
        float s = ImGui::GetTextLineHeight() * 1.3f;
        ImGui::Image(static_cast<ImTextureID>(ic->entAudio), ImVec2(s, s));
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::AlignTextToFramePadding();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, green);
    ImGui::TextUnformatted("オーディオ");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextDisabled("%s", as.spatial ? "3D 空間音" : "2D サウンド");

    ImGui::Spacing();
    ImGui::PushItemWidth(-110.0f);

    BeginEdit(reg, e, m_audioEdit);
    bool changed = false, active = false;

    char buf[256] = {};
    size_t n = as.clipPath.copy(buf, sizeof(buf) - 1);
    buf[n] = '\0';
    if (ImGui::InputText("クリップ Clip", buf, sizeof(buf)))
    { as.clipPath = buf; changed = true; }
    active |= ImGui::IsItemActive();

    changed |= ImGui::SliderFloat("音量 Volume", &as.volume, 0.0f, 1.0f, "%.2f");
    active  |= ImGui::IsItemActive();

    ImGui::SeparatorText("再生");
    changed |= ImGui::Checkbox("開始時に再生 Play On Start", &as.playOnStart);
    ImGui::SameLine(0.0f, 16.0f);
    changed |= ImGui::Checkbox("ループ Loop", &as.loop);

    ImGui::SeparatorText("空間化");
    changed |= ImGui::Checkbox("3D 空間音にする Spatial", &as.spatial);
    if (as.spatial)
    {
        changed |= ImGui::DragFloat("最小距離 Min", &as.minDistance, 0.1f, 0.0f, 1000.0f, "%.1f");
        active  |= ImGui::IsItemActive();
        changed |= ImGui::DragFloat("最大距離 Max", &as.maxDistance, 0.5f, 0.1f, 5000.0f, "%.1f");
        active  |= ImGui::IsItemActive();
        ImGui::TextDisabled("空間化はモノラル wav のみ。位置は Transform。");
    }

    EndEdit(reg, ctx, e, m_audioEdit, changed, active, "Audio Source");
    ImGui::PopItemWidth();
    ImGui::Separator();
}

void InspectorPanel::RenderEngineSettings(EditorContext& ctx,
                                          Camera* camera,
                                          AudioSystem* audioSystem,
                                          PhysicsDebugRenderer* physicsDebugRenderer,
                                          bool& physicsDebugDraw,
                                          bool& useVsync,
                                          i32& shadowQualityIndex,
                                          u32& shadowMapSize,
                                          bool& shadowMapDirty,
                                          f32& cascadeSplitLambda,
                                          f32& cascadeBlendBand,
                                          bool& showCascadeDebug,
                                          GameClock* clock)
{
    ImGui::Begin("\xe3\x82\xa8\xe3\x83\xb3\xe3\x82\xb8\xe3\x83\xb3\xe8\xa8\xad\xe5\xae\x9a");  // エンジン設定

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

        // --- CSM (Cascaded Shadow Maps) ---
        ImGui::Separator();
        ImGui::TextUnformatted("CSM (4\xe5\x88\x86\xe5\x89\xb2)");  // CSM (4分割)
        ImGui::SliderFloat("\xe5\x88\x86\xe5\x89\xb2\xce\xbb", &cascadeSplitLambda, 0.0f, 1.0f);  // 分割λ
        ImGui::SliderFloat("\xe5\xa2\x83\xe7\x95\x8c\xe3\x83\x96\xe3\x83\xac\xe3\x83\xb3\xe3\x83\x89", &cascadeBlendBand, 0.0f, 5.0f);  // 境界ブレンド
        ImGui::Checkbox("\xe3\x82\xab\xe3\x82\xb9\xe3\x82\xb1\xe3\x83\xbc\xe3\x83\x89\xe5\x8f\xaf\xe8\xa6\x96\xe5\x8c\x96", &showCascadeDebug);  // カスケード可視化
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
        if (ImGui::Button("ゲームをビルド..."))  // 配置先フォルダをピッカーで選んでからビルド
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

    ImGui::End();
}

} // namespace dx12e
