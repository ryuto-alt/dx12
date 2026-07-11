#include "editor/panels/InspectorPanel.h"
#include "editor/EditorContext.h"
#include "editor/PropertyGrid.h"
#include "editor/UiEditUtil.h"
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
#include "resource/ShaderRegistry.h"
#include "resource/MaterialAssetIO.h"
#include "editor/panels/AssetBrowserPanel.h"

#include <imgui_internal.h>   // BeginDragDropTargetCustom（ウィンドウ全体をドロップ先に）
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

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

// CollapsingHeader を Unreal 風カテゴリ帯（濃いグレー地＋左端アクセントストライプ）で描くヘルパ。
// tex 指定でラベル頭に種別アイコンを重ね描きする。
bool IconHeader(const dx12e::EditorUiIcons* ic, dx12e::u64 tex, const char* label,
                ImGuiTreeNodeFlags flags = 0)
{
    namespace th = dx12e::theme;
    ImGui::PushStyleColor(ImGuiCol_Header,        th::GroupBg);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, th::Hex(0x272831));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  th::Hex(0x2b2c36));
    ImGui::PushStyleColor(ImGuiCol_Text,          th::TextHi);

    bool open;
    if (!ic || !tex)
    {
        open = ImGui::CollapsingHeader(label, flags);
    }
    else
    {
        const float h = ImGui::GetTextLineHeight();
        const float spaceW = ImGui::CalcTextSize(" ").x;
        const int pad = (spaceW > 0.0f) ? static_cast<int>((h + 6.0f) / spaceW) + 1 : 3;
        std::string padded(static_cast<size_t>(pad), ' ');
        padded += label;
        open = ImGui::CollapsingHeader(padded.c_str(), flags);
    }
    ImGui::PopStyleColor(4);

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    // 左端のアクセントストライプ（カテゴリの目印。Unreal の Details 風）
    ImGui::GetWindowDrawList()->AddRectFilled(
        mn, ImVec2(mn.x + 3.0f, mx.y), ImGui::GetColorU32(th::Accent));

    if (ic && tex)
    {
        const float h = ImGui::GetTextLineHeight();
        const float cy = (mn.y + mx.y) * 0.5f;
        const float x  = mn.x + ImGui::GetTreeNodeToLabelSpacing();
        ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(tex),
            ImVec2(x, cy - h * 0.5f), ImVec2(x + h, cy + h * 0.5f));
    }
    return open;
}

// エンティティの構成から代表アイコンを選ぶ（Inspector 上部の見出し用）
dx12e::u64 PickEntityIcon(entt::registry& reg, entt::entity e, const dx12e::EditorUiIcons& ic)
{
    using namespace dx12e;
    if (reg.all_of<CameraComponent>(e))                       return ic.entCamera;
    if (reg.any_of<PointLight, DirectionalLight, SpotLight>(e)) return ic.entLight;
    if (reg.any_of<UICanvas, UIRect, UIImage, UIText, UIButton, UIAnimator>(e)) return ic.entUi;
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

    bool open = IconHeader(nullptr, 0, "Lua Script",
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

    // ヘッダ右の状態表示: 付いてたら緑、無ければグレー（テキスト幅から右寄せ位置を計算）
    {
        const char* status = hasLua ? "ATTACHED" : "(none)";
        ImGui::SameLine(ImGui::GetWindowWidth()
                        - ImGui::CalcTextSize(status).x
                        - ImGui::GetStyle().WindowPadding.x - 24.0f);
        if (hasLua)
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), status);
        else
            ImGui::TextDisabled(status);
    }

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
    if (dx12e::pg::Begin("LuaScriptHead"))
    {
        dx12e::pg::InputText("Script", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_ReadOnly);
        dx12e::pg::Label("有効 Enabled");
        ImGui::Checkbox("##lsEnabled", &ls.enabled);
        ImGui::SameLine();
        ImGui::TextColored(
            ls.started ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
            ls.started ? "RUNNING" : "IDLE");
        dx12e::pg::End();
    }

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
            if (dx12e::pg::Begin("LuaProps"))
            {
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
                        bool ch = d.hasRange ? dx12e::pg::SliderFloat(lbl, &v, d.minVal, d.maxVal)
                                             : dx12e::pg::Float(lbl, &v, 0.01f);
                        if (ch) p.num = v;
                        break;
                    }
                    case dx12e::ScriptPropType::Int:
                    {
                        int v = static_cast<int>(p.num);
                        bool ch = d.hasRange ? dx12e::pg::SliderInt(lbl, &v, static_cast<int>(d.minVal), static_cast<int>(d.maxVal))
                                             : dx12e::pg::Int(lbl, &v);
                        if (ch) p.num = v;
                        break;
                    }
                    case dx12e::ScriptPropType::Bool:
                        dx12e::pg::Checkbox(lbl, &p.b);
                        break;
                    case dx12e::ScriptPropType::String:
                    {
                        char buf[256];
                        std::memset(buf, 0, sizeof(buf));
                        strncpy_s(buf, sizeof(buf), p.str.c_str(), _TRUNCATE);
                        if (dx12e::pg::InputText(lbl, buf, sizeof(buf)))
                            p.str = buf;
                        break;
                    }
                    case dx12e::ScriptPropType::Vec3:
                        dx12e::pg::Float3(lbl, &p.vec.x, 0.01f);
                        break;
                    case dx12e::ScriptPropType::Color:
                        dx12e::pg::Color3(lbl, &p.vec.x);
                        break;
                    case dx12e::ScriptPropType::Entity:
                    {
                        // シーン内のエンティティ名から選ぶコンボ（参照先を名前で保持）。
                        const char* cur = p.str.empty() ? "(なし)" : p.str.c_str();
                        dx12e::pg::Label(lbl);
                        if (ImGui::BeginCombo("##ent", cur))
                        {
                            if (ImGui::Selectable("(なし)", p.str.empty()))
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
                dx12e::pg::End();
            }
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
    ImGui::SetNextItemWidth(-FLT_MIN);
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

// ※ UIRect の「親矩形」解決（ResolveUiParentRectPx）は SceneView / UIエディタと共用のため
//   editor/UiEditUtil.h（uiedit 名前空間）へ移設した。

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

                bool anyActive = false;
                if (pg::Begin("Transform"))
                {
                    pg::Float3("位置 Position", &t.position.x, 0.1f,  0, 0, "%.3f", &anyActive);
                    pg::Float3("回転 Rotation", &t.rotation.x, 1.0f,  0, 0, "%.2f", &anyActive);
                    pg::Float3("拡縮 Scale",    &t.scale.x,    0.01f, 0, 0, "%.3f", &anyActive);
                    pg::End();
                }
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
                if (pg::Begin("MeshRenderer"))
                {
                    pg::Text("Meshes",    "%d", static_cast<int>(r.meshes.size()));
                    pg::Text("Materials", "%d", static_cast<int>(r.materials.size()));
                    pg::End();
                }
            }
        }

        // Sprite2D（ワールド/HUD スプライト）
        if (reg.all_of<Sprite2D>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "Sprite2D");
            bool removed = ComponentRemoveMenu<Sprite2D>(reg, ctx, ctx.selectedEntity, "Sprite2D");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_spriteEdit);
                auto& sp = reg.get<Sprite2D>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("Sprite2D"))
                {
                    char buf[256] = {};
                    size_t n = sp.texturePath.copy(buf, sizeof(buf) - 1);
                    buf[n] = '\0';
                    if (pg::InputText("テクスチャ Texture", buf, sizeof(buf), 0, &active))
                    { sp.texturePath = buf; changed = true; }

                    changed |= pg::Int("描画順 Layer", &sp.layer, 1.0f, -1000, 1000, &active);
                    changed |= pg::Float2("サイズ Size", &sp.size.x, 0.01f, 0.0f, 100.0f, "%.2f", &active);
                    changed |= pg::Float2("UV Min", &sp.uvMin.x, 0.005f, 0.0f, 1.0f, "%.3f", &active);
                    changed |= pg::Float2("UV Max", &sp.uvMax.x, 0.005f, 0.0f, 1.0f, "%.3f", &active);
                    changed |= pg::Color4("色 Color", &sp.color.x);

                    pg::Group("配置");
                    changed |= pg::Checkbox("ワールド空間 World Space", &sp.worldSpace,
                        "OFF: HUD 表示（画面固定）。位置は Transform の値をピクセル扱い");
                    if (sp.worldSpace)
                    {
                        changed |= pg::Checkbox("常にカメラを向く Billboard", &sp.billboard,
                            "OFF: Transform の回転どおりに固定表示（ギズモの R で回転可）\n"
                            "ON : 常にアクティブカメラの方を向く（回転は無視される）");
                    }
                    pg::End();
                }

                // カスタムシェーダー割当（worldSpace のスプライトのみ有効。MeshRendererの仕組みを踏襲するが
                // ルートシグネチャ/頂点フォーマットが異なるので互換性は無い＝別キャッシュ）。
                if (IconHeader(nullptr, 0, "Shader##Sprite2D") && pg::Begin("Sprite2DShader"))
                {
                    namespace fs = std::filesystem;
                    std::string currentLabel = sp.shaderPath.empty() ? "既定 (Sprite)" : sp.shaderPath;
                    pg::Label("シェーダー");
                    if (ImGui::BeginCombo("##sprShader", currentLabel.c_str()))
                    {
                        std::vector<std::string> options;
                        std::error_code ec;
                        fs::path root(m_assetsDir + "shaders/");
                        if (fs::exists(root, ec))
                        {
                            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
                            fs::recursive_directory_iterator end;
                            for (; !ec && it != end; it.increment(ec))
                            {
                                std::error_code fec;
                                if (!it->is_regular_file(fec) || fec) continue;
                                if (it->path().extension() != L".hlsl") continue;
                                fs::path rel = fs::relative(it->path(), root, fec);
                                if (fec) continue;
                                std::string relStr = rel.generic_string();
                                if (FindShaderSourceByRelPath(relStr) != nullptr)
                                    continue;  // Registry一致=上書き用途。個別割当の選択肢からは除外
                                options.push_back(relStr);
                            }
                        }

                        if (ImGui::Selectable("既定 (Sprite)", sp.shaderPath.empty()))
                        { sp.shaderPath.clear(); changed = true; }
                        for (const auto& opt : options)
                        {
                            if (ImGui::Selectable(opt.c_str(), sp.shaderPath == opt))
                            { sp.shaderPath = opt; changed = true; }
                        }
                        if (options.empty())
                            ImGui::TextDisabled("(assets/shaders/ に自作シェーダーなし)");
                        ImGui::EndCombo();
                    }
                    if (!sp.shaderPath.empty())
                    {
                        changed |= pg::Checkbox("アルファブレンド有効", &sp.shaderAlphaBlend,
                            "ON: シェーダーの alpha 出力を SrcAlpha/InvSrcAlpha でブレンド(DepthWrite OFF)。"
                            "OFF(既定): 不透明固定で alpha は無視される");
                        changed |= pg::Float("エフェクト値 effectValue", &sp.effectValue, 0.005f, 0.0f, 1.0f, "%.3f", &active,
                            "シェーダーへ渡す汎用の進捗/強度値(意味はシェーダー依存)。"
                            "Luaの scene:setSpriteEffect(e, value) で実行時にも変更可");
                        changed |= pg::Float4("パラメーター shaderParams", &sp.shaderParams.x, 0.01f, 0.0f, 0.0f, "%.3f", &active,
                            "シェーダーへ渡す汎用パラメーター4つ(意味はシェーダー依存)。HLSL側は VSIn/PSIn に "
                            "float4 params : TEXCOORD2; を足して読む。Luaの scene:setSpriteParams(e, x,y,z,w) でも変更可");
                    }
                    pg::End();
                    if (!sp.worldSpace && !sp.shaderPath.empty())
                        ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                            "HUD スプライトはカスタムシェーダー未対応(worldSpaceのみ)");
                }

                EndEdit(reg, ctx, ctx.selectedEntity, m_spriteEdit, changed, active, "Sprite2D");
            }
        }

        // UICanvas（ゲーム内UIのルート。子孫の UIRect ツリーを Play 中に描画する）
        if (reg.all_of<UICanvas>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "UICanvas");
            bool removed = ComponentRemoveMenu<UICanvas>(reg, ctx, ctx.selectedEntity, "UICanvas");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiCanvasEdit);
                auto& cv = reg.get<UICanvas>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UICanvas"))
                {
                    changed |= pg::Float("基準幅 Ref Width", &cv.refWidth, 1.0f, 1.0f, 16384.0f, "%.0f", &active,
                        "レイアウトの基準解像度。UI はこの解像度で設計し、実行時に画面へ合わせる");
                    changed |= pg::Float("基準高さ Ref Height", &cv.refHeight, 1.0f, 1.0f, 16384.0f, "%.0f", &active);
                    static const char* scaleModes[] = {
                        "等比スケール（中央寄せレターボックス）",
                        "実ピクセル（左上原点・等倍）",
                    };
                    changed |= pg::Combo("スケールモード Scale Mode", &cv.scaleMode, scaleModes, 2,
                        "等比スケール: 基準解像度をアスペクト比を保って画面に収める\n"
                        "実ピクセル: スケールせず左上原点の実ピクセルで配置する");
                    changed |= pg::Int("描画順 Sort Order", &cv.sortOrder, 1.0f, -100, 100, &active,
                        "キャンバス間の描画順（小さいほど奥）");
                    changed |= pg::Checkbox("表示 Visible", &cv.visible,
                        "OFF: このキャンバス配下の UI をすべて描画しない");
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiCanvasEdit, changed, active, "UICanvas");
            }
        }

        // UIRect（UI レイアウトノード。アンカー＋オフセットで親矩形に追従する）
        if (reg.all_of<UIRect>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "UIRect");
            bool removed = ComponentRemoveMenu<UIRect>(reg, ctx, ctx.selectedEntity, "UIRect");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiRectEdit);
                auto& ur = reg.get<UIRect>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIRect"))
                {
                    // ── アンカープリセット（Unity 風 4x4 = 9方位＋ストレッチ）──
                    // 選択時は解決済み矩形（見た目の位置）を維持するよう offset を補正してから
                    // anchorMin/Max を差し替える。親矩形サイズはキャンバス基準解像度で解決する。
                    pg::Label("アンカー Anchor",
                        "親矩形のどこに追従するか。プリセット選択時は見た目の位置を保ったまま\n"
                        "アンカーとオフセットを再計算する。最下行/最右列はストレッチ");
                    {
                        // 軸ごとの候補: {anchorMin, anchorMax}。0=左/上 0.5=中央 1=右/下 {0,1}=ストレッチ
                        static const float kAxis[4][2] = {
                            {0.0f, 0.0f}, {0.5f, 0.5f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
                        static const char* kColName[4] = {"左", "中央", "右", "横ストレッチ"};
                        static const char* kRowName[4] = {"上", "中央", "下", "縦ストレッチ"};

                        DirectX::XMFLOAT2 pMin{0.0f, 0.0f};
                        DirectX::XMFLOAT2 pMax{1920.0f, 1080.0f};   // UI ツリー外は既定値で近似
                        uiedit::ResolveUiParentRectPx(reg, ctx.selectedEntity, pMin, pMax);
                        const float pw = pMax.x - pMin.x;
                        const float ph = pMax.y - pMin.y;

                        ImGui::PushID("AnchorPreset");
                        auto* dl = ImGui::GetWindowDrawList();
                        const float cell = ImGui::GetFrameHeight();
                        for (int row = 0; row < 4; ++row)
                        {
                            for (int col = 0; col < 4; ++col)
                            {
                                if (col) ImGui::SameLine(0.0f, 3.0f);
                                ImGui::PushID(row * 4 + col);
                                const bool current =
                                    std::fabs(ur.anchorMin.x - kAxis[col][0]) < 1e-4f &&
                                    std::fabs(ur.anchorMax.x - kAxis[col][1]) < 1e-4f &&
                                    std::fabs(ur.anchorMin.y - kAxis[row][0]) < 1e-4f &&
                                    std::fabs(ur.anchorMax.y - kAxis[row][1]) < 1e-4f;
                                if (ImGui::Button("##anchor", ImVec2(cell, cell)))
                                {
                                    // 見た目の位置を維持: 新旧アンカー差 × 親サイズぶん offset を補正
                                    ur.offsetMin.x += pw * (ur.anchorMin.x - kAxis[col][0]);
                                    ur.offsetMax.x += pw * (ur.anchorMax.x - kAxis[col][1]);
                                    ur.offsetMin.y += ph * (ur.anchorMin.y - kAxis[row][0]);
                                    ur.offsetMax.y += ph * (ur.anchorMax.y - kAxis[row][1]);
                                    ur.anchorMin = {kAxis[col][0], kAxis[row][0]};
                                    ur.anchorMax = {kAxis[col][1], kAxis[row][1]};
                                    changed = true;
                                }
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip("%s・%s", kRowName[row], kColName[col]);

                                // ミニ図: 外枠=親矩形、塗り=アンカーの位置/範囲
                                const ImVec2 bmin = ImGui::GetItemRectMin();
                                const ImVec2 bmax = ImGui::GetItemRectMax();
                                dl->AddRect(ImVec2(bmin.x + 2.0f, bmin.y + 2.0f),
                                            ImVec2(bmax.x - 2.0f, bmax.y - 2.0f),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled));
                                const float innerW = bmax.x - bmin.x - 6.0f;
                                const float innerH = bmax.y - bmin.y - 6.0f;
                                float x0 = bmin.x + 3.0f + innerW * kAxis[col][0];
                                float x1 = bmin.x + 3.0f + innerW * kAxis[col][1];
                                float y0 = bmin.y + 3.0f + innerH * kAxis[row][0];
                                float y1 = bmin.y + 3.0f + innerH * kAxis[row][1];
                                const float dot = 2.0f;   // 点アンカーの最低表示幅（半分）
                                if (x1 - x0 < dot * 2.0f) { const float c = (x0 + x1) * 0.5f; x0 = c - dot; x1 = c + dot; }
                                if (y1 - y0 < dot * 2.0f) { const float c = (y0 + y1) * 0.5f; y0 = c - dot; y1 = c + dot; }
                                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                                    ImGui::GetColorU32(current ? theme::Accent
                                                               : ImGui::GetStyleColorVec4(ImGuiCol_Text)));
                                ImGui::PopID();
                            }
                        }
                        ImGui::PopID();
                    }

                    // ── 配置テンプレ（9 方位）: アンカー + 位置を同時スナップ ──
                    // アンカープリセットが「アンカーだけ変える（見た目は保つ）」のに対し、こちらは
                    // 要素そのものを親矩形のその方位へピッタリ寄せる（Unity の Alt+プリセット相当）。
                    pg::Label("配置 Placement",
                        "要素を親矩形の 9 方位へスナップ配置する。アンカーも同時に設定されるので\n"
                        "解像度が変わってもその方位に追従する（サイズは維持）");
                    {
                        static const char* kPlaceLabels[3][3] = {
                            {"\xe2\x86\x96", "\xe2\x86\x91", "\xe2\x86\x97"},   // ↖ ↑ ↗
                            {"\xe2\x86\x90", "\xe2\x97\x8f", "\xe2\x86\x92"},   // ← ● →
                            {"\xe2\x86\x99", "\xe2\x86\x93", "\xe2\x86\x98"},   // ↙ ↓ ↘
                        };
                        static const char* kPlaceNames[3][3] = {
                            {"左上", "上", "右上"},
                            {"左",   "中央", "右"},
                            {"左下", "下", "右下"},
                        };
                        ImGui::PushID("UiPlacement");
                        const float cell = ImGui::GetFrameHeight() + 4.0f;
                        for (int row = 0; row < 3; ++row)
                        {
                            for (int col = 0; col < 3; ++col)
                            {
                                if (col) ImGui::SameLine(0.0f, 3.0f);
                                ImGui::PushID(row * 3 + col);
                                if (ImGui::Button(kPlaceLabels[row][col], ImVec2(cell, cell)))
                                {
                                    if (uiedit::ApplyUiPlacement(reg, ctx.selectedEntity, col, row))
                                        changed = true;
                                }
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip("%s に配置", kPlaceNames[row][col]);
                                ImGui::PopID();
                            }
                        }
                        ImGui::PopID();
                    }

                    changed |= pg::Float2("アンカー Min", &ur.anchorMin.x, 0.005f, 0.0f, 1.0f, "%.3f", &active,
                        "親矩形内の正規化位置（0=左/上 1=右/下）。直接編集では位置補正しない");
                    changed |= pg::Float2("アンカー Max", &ur.anchorMax.x, 0.005f, 0.0f, 1.0f, "%.3f", &active);
                    changed |= pg::Float2("ピボット Pivot", &ur.pivot.x, 0.005f, 0.0f, 1.0f, "%.3f", &active,
                        "位置 Position が指す自分の基準点（0.5,0.5=中心）");

                    const bool anchorsMatch =
                        std::fabs(ur.anchorMax.x - ur.anchorMin.x) < 1e-4f &&
                        std::fabs(ur.anchorMax.y - ur.anchorMin.y) < 1e-4f;
                    if (anchorsMatch)
                    {
                        // アンカー一致: 位置(px)＋サイズ(px)で編集（offset へ換算して保持）
                        DirectX::XMFLOAT2 size{ur.offsetMax.x - ur.offsetMin.x,
                                               ur.offsetMax.y - ur.offsetMin.y};
                        DirectX::XMFLOAT2 pos{ur.offsetMin.x + size.x * ur.pivot.x,
                                              ur.offsetMin.y + size.y * ur.pivot.y};
                        bool rectCh = false;
                        rectCh |= pg::Float2("位置 Position", &pos.x, 1.0f, 0.0f, 0.0f, "%.0f", &active,
                            "アンカー点からのピボット位置(px)");
                        rectCh |= pg::Float2("サイズ Size", &size.x, 1.0f, 0.0f, 32768.0f, "%.0f", &active);
                        if (rectCh)
                        {
                            ur.offsetMin = {pos.x - size.x * ur.pivot.x,
                                            pos.y - size.y * ur.pivot.y};
                            ur.offsetMax = {pos.x + size.x * (1.0f - ur.pivot.x),
                                            pos.y + size.y * (1.0f - ur.pivot.y)};
                            changed = true;
                        }
                    }
                    else
                    {
                        // ストレッチ: アンカー辺からのオフセット(px)を直接編集
                        changed |= pg::Float2("オフセット Min", &ur.offsetMin.x, 1.0f, 0.0f, 0.0f, "%.0f", &active,
                            "アンカー Min 点からのオフセット(px)");
                        changed |= pg::Float2("オフセット Max", &ur.offsetMax.x, 1.0f, 0.0f, 0.0f, "%.0f", &active,
                            "アンカー Max 点からのオフセット(px)");
                    }

                    changed |= pg::Checkbox("表示 Visible", &ur.visible,
                        "OFF: 自分と子孫を描画しない（ボタンも反応しない）");
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiRectEdit, changed, active, "UIRect");
            }
        }

        // UIImage（UI の画像/単色矩形。UIButton があれば状態色が乗算される）
        if (reg.all_of<UIImage>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "UIImage");
            bool removed = ComponentRemoveMenu<UIImage>(reg, ctx, ctx.selectedEntity, "UIImage");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiImageEdit);
                auto& img = reg.get<UIImage>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIImage"))
                {
                    char buf[256] = {};
                    size_t n = img.texturePath.copy(buf, sizeof(buf) - 1);
                    buf[n] = '\0';
                    if (pg::InputText("テクスチャ Texture", buf, sizeof(buf), 0, &active,
                        "assets 相対パス。空なら単色塗り矩形。アセットブラウザから D&D で割当可。\n"
                        "ロードに失敗した場合は単色矩形で代替表示される"))
                    { img.texturePath = buf; changed = true; }

                    // アセットブラウザからの D&D（テクスチャのみ受理、assets 相対パスへ変換）
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* payload =
                                ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
                        {
                            const char* droppedPath = static_cast<const char*>(payload->Data);
                            namespace fs = std::filesystem;
                            std::string ext = fs::path(droppedPath).extension().string();
                            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                            if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Texture)
                            {
                                std::string abs = fs::path(droppedPath).lexically_normal().string();
                                std::string base = fs::path(m_assetsDir).lexically_normal().string();
                                std::replace(abs.begin(), abs.end(), '\\', '/');
                                std::replace(base.begin(), base.end(), '\\', '/');
                                img.texturePath = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
                                changed = true;
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }

                    changed |= pg::Color4("色 Color", &img.color.x);
                    changed |= pg::Float2("UV Min", &img.uvMin.x, 0.005f, 0.0f, 1.0f, "%.3f", &active);
                    changed |= pg::Float2("UV Max", &img.uvMax.x, 0.005f, 0.0f, 1.0f, "%.3f", &active);
                    changed |= pg::FloatN("9スライス境界 Slice Border", &img.sliceBorder.x, 4, 0.5f,
                        0.0f, 4096.0f, "%.0f", &active,
                        "元テクスチャ上の境界px（左,上,右,下）。全0で無効。\n"
                        "角は固定サイズのまま、辺と中央だけ引き伸ばして描画する");
                    changed |= pg::Float("角丸半径 Corner Radius", &img.cornerRadius, 0.5f, 0.0f, 1024.0f,
                        "%.0f", &active, "単色矩形（テクスチャ未指定）のときのみ有効");
                    changed |= pg::Checkbox("クリックを遮る Raycast Block", &img.raycastBlock,
                        "ON: この画像が背後にあるボタンへのクリック/ホバーを遮る（Unity の raycastTarget 相当）。\n"
                        "OFF: クリックを素通しする（装飾用オーバーレイ向け）");
                    changed |= pg::Float("表示割合 Fill Amount", &img.fillAmount, 0.005f, 0.0f, 1.0f,
                        "%.3f", &active,
                        "0〜1 の割合だけ表示する（HPバー/ゲージ用）。1=全表示、0=非表示。\n"
                        "Lua からは scene:setUiFill(entity, amount) で更新できる");
                    {
                        static const char* fillDirs[] = {"左から", "右から", "下から", "上から"};
                        changed |= pg::Combo("Fill 方向 Fill Dir", &img.fillDir, fillDirs, IM_ARRAYSIZE(fillDirs),
                            "Fill Amount が増えるとき、どの端から現れていくか");
                    }
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiImageEdit, changed, active, "UIImage");
            }
        }

        // UIText（UI テキスト。ImGui 共有フォントのスケール描画）
        if (reg.all_of<UIText>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "UIText");
            bool removed = ComponentRemoveMenu<UIText>(reg, ctx, ctx.selectedEntity, "UIText");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiTextEdit);
                auto& txt = reg.get<UIText>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIText"))
                {
                    char buf[1024] = {};
                    size_t n = txt.text.copy(buf, sizeof(buf) - 1);
                    buf[n] = '\0';
                    pg::Label("テキスト Text");
                    if (ImGui::InputTextMultiline("##uiTextValue", buf, sizeof(buf),
                                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f)))
                    { txt.text = buf; changed = true; }
                    active |= ImGui::IsItemActive();

                    changed |= pg::Float("フォントサイズ Font Size", &txt.fontSize, 0.5f, 4.0f, 512.0f,
                                         "%.0f", &active, "キャンバス基準解像度でのpx。実行時にスケールされる");
                    changed |= pg::Color4("色 Color", &txt.color.x);
                    static const char* alignHItems[] = {"左", "中央", "右"};
                    static const char* alignVItems[] = {"上", "中央", "下"};
                    changed |= pg::Combo("水平整列 Align H", &txt.alignH, alignHItems, 3);
                    changed |= pg::Combo("垂直整列 Align V", &txt.alignV, alignVItems, 3);
                    changed |= pg::Checkbox("折り返し Wrap", &txt.wrap, "ON: UIRect の幅で折り返す");
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiTextEdit, changed, active, "UIText");
            }
        }

        // UIButton（クリックで events へ emit。同一エンティティの UIImage を状態色でティント）
        if (reg.all_of<UIButton>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "UIButton");
            bool removed = ComponentRemoveMenu<UIButton>(reg, ctx, ctx.selectedEntity, "UIButton");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiButtonEdit);
                auto& btn = reg.get<UIButton>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIButton"))
                {
                    char buf[128] = {};
                    size_t n = btn.onClickEvent.copy(buf, sizeof(buf) - 1);
                    buf[n] = '\0';
                    if (pg::InputText("クリックイベント名 On Click", buf, sizeof(buf), 0, &active,
                        "クリック確定時に events へ emit するイベント名。空なら発火しない。\n"
                        "Lua 側は events:on(\"イベント名\", function(e) ... end) で受ける"))
                    { btn.onClickEvent = buf; changed = true; }

                    changed |= pg::Color4("通常色 Normal", &btn.normalColor.x);
                    changed |= pg::Color4("ホバー色 Hover", &btn.hoverColor.x);
                    changed |= pg::Color4("押下色 Pressed", &btn.pressedColor.x);
                    changed |= pg::Checkbox("操作可能 Interactable", &btn.interactable,
                        "OFF: 入力を受け付けず通常色で固定");

                    pg::Group("効果音");
                    {
                        char sbuf[256] = {};
                        size_t sn = btn.hoverSound.copy(sbuf, sizeof(sbuf) - 1);
                        sbuf[sn] = '\0';
                        if (pg::InputText("ホバー音 Hover SFX", sbuf, sizeof(sbuf), 0, &active,
                            "カーソル/フォーカスが乗った瞬間に 1 回鳴らす wav\n"
                            "(AudioSource のクリップと同じ assets 相対パス。空=鳴らさない)"))
                        { btn.hoverSound = sbuf; changed = true; }
                        sn = btn.clickSound.copy(sbuf, sizeof(sbuf) - 1);
                        sbuf[sn] = '\0';
                        if (pg::InputText("クリック音 Click SFX", sbuf, sizeof(sbuf), 0, &active,
                            "クリック確定(ボタン上で離した)時に 1 回鳴らす wav"))
                        { btn.clickSound = sbuf; changed = true; }
                    }
                    pg::End();
                }
                if (!reg.all_of<UIImage>(ctx.selectedEntity))
                    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                        "状態色の反映には同一エンティティの UIImage が必要");
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiButtonEdit, changed, active, "UIButton");
            }
        }

        // UIAnimator（UI の出現/ホバー/ループアニメ。Play 中のみ再生。効果は自分と子孫に掛かる）
        if (reg.all_of<UIAnimator>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entUi : 0, "UIAnimator");
            bool removed = ComponentRemoveMenu<UIAnimator>(reg, ctx, ctx.selectedEntity, "UIAnimator");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_uiAnimatorEdit);
                auto& an = reg.get<UIAnimator>(ctx.selectedEntity);
                bool changed = false, active = false;

                if (pg::Begin("UIAnimator"))
                {
                    static const char* showAnims[] = {"なし", "フェード", "ポップ(拡大)",
                                                      "左から", "右から", "上から", "下から"};
                    static const char* easings[] = {"リニア", "イーズイン", "イーズアウト",
                                                    "イン/アウト", "バック(勢い)", "バウンス", "弾性"};
                    static const char* loops[] = {"なし", "浮遊(上下)", "パルス(拡縮)", "点滅"};

                    pg::Label("出現 Show",
                        "Play 開始時と Lua scene:showUi() で再生される出現アニメ。\n"
                        "scene:hideUi() は逆再生で消す");
                    changed |= pg::Combo("出現アニメ Show Anim", &an.showAnim,
                                         showAnims, IM_ARRAYSIZE(showAnims));
                    changed |= pg::Float("時間 Duration", &an.showDuration, 0.01f, 0.05f, 5.0f,
                                         "%.2f", &active);
                    changed |= pg::Float("遅延 Delay", &an.showDelay, 0.01f, 0.0f, 10.0f, "%.2f", &active,
                        "再生開始までの秒。複数の UI をずらして順番に出すのに使う");
                    changed |= pg::Combo("イージング Easing", &an.showEasing,
                                         easings, IM_ARRAYSIZE(easings));
                    changed |= pg::Float("スライド距離 Slide", &an.slideOffset, 1.0f, 0.0f, 2000.0f,
                                         "%.0f", &active, "「〜から」系のときの移動距離(px)");

                    pg::Label("ホバー/押下 Hover",
                        "同一エンティティに UIButton がある時だけ効く（ボタンの気持ちよさ用）");
                    changed |= pg::Float("ホバー拡大 Hover Scale", &an.hoverScale, 0.005f, 0.5f, 2.0f,
                                         "%.2f", &active);
                    changed |= pg::Float("押下縮小 Press Scale", &an.pressScale, 0.005f, 0.5f, 2.0f,
                                         "%.2f", &active);
                    changed |= pg::Float("追従速度 Speed", &an.hoverSpeed, 0.1f, 1.0f, 40.0f,
                                         "%.0f", &active, "大きいほどキビキビ反応する");

                    pg::Label("ループ Loop", "常時再生されるループアニメ（注目させたい UI に）");
                    changed |= pg::Combo("ループアニメ Loop Anim", &an.loopAnim,
                                         loops, IM_ARRAYSIZE(loops));
                    changed |= pg::Float("速さ Speed (Hz)", &an.loopSpeed, 0.01f, 0.05f, 10.0f,
                                         "%.2f", &active);
                    changed |= pg::Float("量 Amount", &an.loopAmount, 0.1f, 0.0f, 200.0f,
                                         "%.2f", &active, "浮遊=px / パルス・点滅=割合(0.05 = ±5%)");
                    pg::End();
                }
                ImGui::TextDisabled("アニメは Play 中に再生されます（エディタ上は最終ポーズ）");
                EndEdit(reg, ctx, ctx.selectedEntity, m_uiAnimatorEdit, changed, active, "UIAnimator");
            }
        }

        // SkeletalAnimation
        if (reg.all_of<SkeletalAnimation>(ctx.selectedEntity))
        {
            auto& skelAnim = reg.get<SkeletalAnimation>(ctx.selectedEntity);
            if (skelAnim.animator && IconHeader(ic, ic ? ic->entMesh : 0, "SkeletalAnimation"))
            {
                if (pg::Begin("SkeletalAnimation"))
                {
                    pg::Text("Bones", "%d",
                        static_cast<int>(skelAnim.skeleton ? skelAnim.skeleton->GetBoneCount() : 0));
                    pg::Text("Clips", "%d", static_cast<int>(skelAnim.clips.size()));
                    pg::End();
                }

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
                if (pg::Begin("GridPlane"))
                {
                    pg::Checkbox("有効 Enabled", &gp.enabled);
                    pg::End();
                }
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
                if (pg::Begin("Gimmick"))
                {
                    changed |= pg::Combo("種類 Kind", &gm.kind, kinds, IM_ARRAYSIZE(kinds));
                    if (gm.kind < 0) gm.kind = 0; if (gm.kind > 3) gm.kind = 3;
                    if (gm.kind != 0)  // Static 以外は動きパラメータを表示
                    {
                        changed |= pg::Float("周期 Period(s)", &gm.period, 0.05f, 0.2f, 30.0f, "%.2f", &active);
                        changed |= pg::SliderFloat("位相 Phase", &gm.phase, 0.0f, 1.0f, "%.2f", &active);
                        changed |= pg::Float("振幅 Amplitude", &gm.amplitude, 0.05f, 0.0f, 30.0f, "%.2f", &active);
                    }
                    if (gm.kind == 1)  // SpikePulse 固有
                    {
                        changed |= pg::SliderFloat("塞ぐ閾値 Threshold", &gm.threshold, 0.0f, 1.0f, "%.2f", &active);
                        changed |= pg::Checkbox("直撃死 Deadly", &gm.deadly);
                    }
                    changed |= pg::Checkbox("当たり判定 Solid", &gm.solid);
                    pg::End();
                }
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
                const char* blends[] = { "加算 Additive", "アルファ Alpha" };
                if (pg::Begin("ParticleEmitter"))
                {
                    changed |= pg::Combo("見た目 Kind", &pe.kind, kinds, IM_ARRAYSIZE(kinds));
                    changed |= pg::Combo("合成 Blend", &pe.blend, blends, IM_ARRAYSIZE(blends));
                    {
                        const char* orients[] = { "ビルボード(カメラ正対)", "水平(地面向き)", "垂直(+Z正対)" };
                        changed |= pg::Combo("向き Orient", &pe.orient, orients, IM_ARRAYSIZE(orients),
                            "粒子クアッドの向き。ビルボード=常にカメラを向く(従来)。\n"
                            "水平=XZ平面(リング/衝撃波/魔法陣)。垂直=XY平面固定。\n"
                            "stretch>0 の速度ストレッチ時と GPUパーティクルでは無効");
                    }
                    {
                        static char texBuf[260] = "";
                        pg::Label("テクスチャ", "assetsからの相対パス。空=プロシージャル質感");
                        ImGui::InputTextWithHint("##peTex", "空=プロシージャル質感", texBuf, sizeof(texBuf));
                        if (ImGui::IsItemDeactivatedAfterEdit()) { pe.texturePath = texBuf; changed = true; }
                        if (!ImGui::IsItemActive() && pe.texturePath != texBuf)
                        {
                            size_t n = pe.texturePath.size();
                            if (n >= sizeof(texBuf)) n = sizeof(texBuf) - 1;
                            std::memcpy(texBuf, pe.texturePath.c_str(), n);
                            texBuf[n] = '\0';
                        }
                    }
                    changed |= pg::Checkbox("GPUパーティクル", &pe.gpu,
                        "compute シムで最大 131072 粒子（加算専用・大量粒子向け）。\n歪み/ライト/中間サイズ/アルファ合成は CPU 専用のため無効");
                    changed |= pg::Float("放出レート Rate(/s)", &pe.rate, 0.5f, 0.0f, pe.gpu ? 100000.0f : 500.0f, "%.1f", &active);
                    changed |= pg::Checkbox("Play開始で放出", &pe.playOnStart);
                    changed |= pg::Checkbox("ループ Looping", &pe.looping);
                    if (!pe.looping)
                        changed |= pg::Float("継続秒 Duration", &pe.duration, 0.05f, 0.0f, 60.0f, "%.2f", &active);

                    pg::Group("見た目");
                    changed |= pg::Color3("開始色 Color", &pe.color.x);
                    changed |= pg::Checkbox("中間色を使う", &pe.hasColorMid);
                    if (pe.hasColorMid)
                        changed |= pg::Color3("中間色 ColorMid", &pe.colorMid.x);
                    changed |= pg::Color3("終了色 ColorEnd", &pe.colorEnd.x);
                    changed |= pg::Float("輝度 Intensity", &pe.intensity, 0.05f, 0.0f, 30.0f, "%.2f", &active);
                    changed |= pg::Float("サイズ Size", &pe.size, 0.01f, 0.0f, 10.0f, "%.3f", &active);
                    changed |= pg::Float("中間サイズ SizeMid", &pe.sizeMid, 0.01f, -1.0f, 10.0f, "%.3f", &active,
                        "-1で無効。0以上で 開始→中間→終了 の3キーサイズカーブ");
                    changed |= pg::Float("終了サイズ SizeEnd", &pe.sizeEnd, 0.01f, 0.0f, 10.0f, "%.3f", &active);
                    changed |= pg::Float("寿命 Life(s)", &pe.life, 0.01f, 0.01f, 30.0f, "%.2f", &active);
                    changed |= pg::SliderFloat("寿命ばらつき", &pe.lifeVar, 0.0f, 1.0f, "%.2f", &active);

                    pg::Group("動き");
                    changed |= pg::Float3("方向 Dir", &pe.dir.x, 0.01f, 0, 0, "%.2f", &active);
                    changed |= pg::SliderFloat("拡がり Spread", &pe.spread, 0.0f, 1.0f, "%.2f", &active);
                    changed |= pg::Float("速度 Speed", &pe.speed, 0.02f, 0.0f, 50.0f, "%.2f", &active);
                    changed |= pg::SliderFloat("速度ばらつき", &pe.speedVar, 0.0f, 1.0f, "%.2f", &active);
                    changed |= pg::Float("重力 Gravity", &pe.gravity, 0.02f, -50.0f, 50.0f, "%.2f", &active);
                    changed |= pg::Float("抵抗 Drag", &pe.drag, 0.02f, 0.0f, 10.0f, "%.2f", &active);
                    changed |= pg::Float("上向き Up", &pe.up, 0.02f, 0.0f, 10.0f, "%.2f", &active);
                    changed |= pg::Float("ストレッチ Stretch", &pe.stretch, 0.02f, 0.0f, 10.0f, "%.2f", &active);
                    changed |= pg::Float("乱流 Turbulence", &pe.turbStrength, 0.01f, 0.0f, 10.0f, "%.2f", &active,
                        ">0 でカールノイズによる有機的な揺らぎ（煙/炎向け）");
                    if (pe.turbStrength > 0.0f)
                        changed |= pg::Float("乱流の細かさ", &pe.turbFreq, 0.01f, 0.01f, 10.0f, "%.2f", &active);

                    pg::Group("特殊効果");
                    changed |= pg::SliderFloat("画面歪み Distort", &pe.distort, 0.0f, 3.0f, "%.2f", &active,
                        ">0 で色でなく画面の歪みを描く（熱ゆらぎ。Kind=Ring で衝撃波）");
                    changed |= pg::Checkbox("ライト放出 Light", &pe.light,
                        "明るい粒子の上位数個が実ポイントライトになり周囲を照らす（炎/魔法）");
                    if (pe.light)
                        changed |= pg::Float("光の距離 Range", &pe.lightRange, 0.05f, 0.1f, 50.0f, "%.2f", &active);
                    changed |= pg::SliderFloat("明滅 Flicker", &pe.flicker, 0.0f, 1.0f, "%.2f", &active);
                    if (pe.flicker > 0.0f)
                        changed |= pg::Float("明滅の速さ", &pe.flickerFreq, 0.2f, 0.1f, 60.0f, "%.1f", &active);
                    pg::End();
                }
                ImGui::TextDisabled("エディタでもプレビュー表示されます。詳細な作成は ツール > パーティクルエディタ が便利です");
                EndEdit(reg, ctx, ctx.selectedEntity, m_emitterEdit, changed, active, "Particle Emitter");
            }
        }

        // TrailRenderer（軌跡リボン: 剣の残像/弾道/魔法の尾）
        if (reg.all_of<TrailRenderer>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entMesh : 0, "Trail Renderer");
            bool removed = ComponentRemoveMenu<TrailRenderer>(reg, ctx, ctx.selectedEntity, "Trail Renderer");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_trailEdit);
                auto& tr = reg.get<TrailRenderer>(ctx.selectedEntity);
                bool changed = false, active = false;
                const char* tblends[] = { "加算 Additive", "アルファ Alpha" };
                if (pg::Begin("TrailRenderer"))
                {
                    changed |= pg::Checkbox("記録中 Emitting", &tr.emitting);
                    changed |= pg::Float("幅 Width", &tr.width, 0.01f, 0.01f, 10.0f, "%.3f", &active);
                    changed |= pg::Float("寿命 Life(s)", &tr.life, 0.02f, 0.05f, 10.0f, "%.2f", &active);
                    changed |= pg::Color3("先頭色 Color", &tr.color.x);
                    changed |= pg::Color3("尾の色 ColorEnd", &tr.colorEnd.x);
                    changed |= pg::Float("輝度 Intensity", &tr.intensity, 0.05f, 0.0f, 30.0f, "%.2f", &active);
                    changed |= pg::Combo("合成 Blend", &tr.blend, tblends, IM_ARRAYSIZE(tblends));
                    changed |= pg::Float("最小移動 MinDist", &tr.minDist, 0.005f, 0.001f, 2.0f, "%.3f", &active);
                    pg::End();
                }
                ImGui::TextDisabled("エンティティを動かすと軌跡の帯が出ます（エディタでもプレビュー）");
                EndEdit(reg, ctx, ctx.selectedEntity, m_trailEdit, changed, active, "Trail Renderer");
            }
        }

        // NetworkIdentity（マルチプレイ複製対象の印。netId/owner はランタイム表示のみ）
        if (reg.all_of<NetworkIdentity>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "Network Identity");
            bool removed = ComponentRemoveMenu<NetworkIdentity>(reg, ctx, ctx.selectedEntity, "Network Identity");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_netIdEdit);
                auto& ni = reg.get<NetworkIdentity>(ctx.selectedEntity);
                bool changed = false, active = false;
                if (pg::Begin("NetworkIdentity"))
                {
                    changed |= pg::Float("関連半径 InterestRadius", &ni.interestRadius, 0.5f, 0.0f, 1000.0f, "%.1f", &active,
                        "0 = 常に全クライアントへ複製（フェーズ⑧の興味管理で使用予定）");
                    changed |= pg::Checkbox("サーバー権威", &ni.serverAuthority);
                    pg::Text("netId（実行時割当）", "%d", static_cast<int>(ni._netId));
                    pg::Text("owner clientId", "%d", static_cast<int>(ni._owner));
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_netIdEdit, changed, active, "Network Identity");
            }
        }

        // NetworkTransform（Transformのスナップショット複製設定。NetworkIdentityと併用）
        if (reg.all_of<NetworkTransform>(ctx.selectedEntity))
        {
            bool open = IconHeader(ic, ic ? ic->entEmpty : 0, "Network Transform");
            bool removed = ComponentRemoveMenu<NetworkTransform>(reg, ctx, ctx.selectedEntity, "Network Transform");
            if (open && !removed)
            {
                BeginEdit(reg, ctx.selectedEntity, m_netTfEdit);
                auto& nt = reg.get<NetworkTransform>(ctx.selectedEntity);
                bool changed = false, active = false;
                const char* modes[] = { "補間 Interpolated", "オーナー予測 Predicted(未実装)" };
                if (pg::Begin("NetworkTransform"))
                {
                    changed |= pg::Combo("同期モード SyncMode", &nt.syncMode, modes, IM_ARRAYSIZE(modes));
                    pg::Label("同期する要素");
                    changed |= ImGui::Checkbox("位置", &nt.syncPosition);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("回転", &nt.syncRotation);
                    ImGui::SameLine();
                    changed |= ImGui::Checkbox("拡縮", &nt.syncScale);
                    changed |= pg::Float("補間遅延 (ms)", &nt.interpDelayMs, 1.0f, 0.0f, 1000.0f, "%.0f", &active,
                        "受信スナップショットをこの時間だけ遅らせて補間する(ジッター吸収)");
                    changed |= pg::Float("テレポート距離", &nt.snapDistance, 0.1f, 0.0f, 100.0f, "%.1f", &active);
                    pg::End();
                }
                EndEdit(reg, ctx, ctx.selectedEntity, m_netTfEdit, changed, active, "Network Transform");
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
                if (pg::Begin("Trigger"))
                {
                    pg::Combo("形 Shape", &tr.shape, shapes, IM_ARRAYSIZE(shapes));
                    if (tr.shape == 0) pg::Float3("半径 HalfExtents", &tr.halfExtents.x, 0.05f, 0.0f, 1000.0f, "%.2f");
                    else               pg::Float("半径 Radius", &tr.radius, 0.05f, 0.0f, 1000.0f, "%.2f");
                    pg::Float3("オフセット Offset", &tr.offset.x, 0.05f, 0, 0, "%.2f");
                    {
                        const char* cur = tr.filter.empty() ? "Player（既定）" : tr.filter.c_str();
                        pg::Label("対象 Filter");
                        if (ImGui::BeginCombo("##trFilter", cur))
                        {
                            if (ImGui::Selectable("Player（既定）", tr.filter.empty())) tr.filter.clear();
                            auto nv = reg.view<dx12e::NameTag>();
                            for (auto ne : nv)
                            { const auto& nm = nv.get<dx12e::NameTag>(ne).name; if (nm.empty()) continue;
                              if (ImGui::Selectable(nm.c_str(), nm == tr.filter)) tr.filter = nm; }
                            ImGui::EndCombo();
                        }
                    }
                    pg::Checkbox("一度だけ Once", &tr.once);
                    pg::End();
                }
                ImGui::SeparatorText("アクション");
                int removeIdx = -1;
                for (size_t i = 0; i < tr.actions.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    auto& a = tr.actions[i];
                    if (pg::Begin("TriggerAction"))
                    {
                        const char* whens[] = { "入った時 Enter", "出た時 Exit", "居る間 Stay" };
                        pg::Combo("いつ When", &a.when, whens, IM_ARRAYSIZE(whens));
                        const char* types[] = { "Enable", "Disable", "Destroy", "Move", "PlayEffect",
                                                "StopEffect", "PlaySound", "LoadScene", "FadeToScene",
                                                "SetProperty", "EmitEvent" };
                        pg::Combo("何を Type", &a.type, types, IM_ARRAYSIZE(types));
                        {
                            const char* cur = a.target.empty() ? "(なし=Filter対象)" : a.target.c_str();
                            pg::Label("対象 Target");
                            if (ImGui::BeginCombo("##actTarget", cur))
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
                        if (a.type == 3) pg::Float3("移動量 Vec", &a.vec.x, 0.05f, 0, 0, "%.2f");
                        if (a.type == 7 || a.type == 8)
                        { std::memset(buf, 0, sizeof(buf)); strncpy_s(buf, sizeof(buf), a.str.c_str(), _TRUNCATE);
                          if (pg::InputText("シーン Path", buf, sizeof(buf))) a.str = buf; }
                        if (a.type == 8)
                        { float d = static_cast<float>(a.num); if (pg::Float("秒 Dur", &d, 0.05f, 0.0f, 10.0f, "%.2f")) a.num = d; }
                        if (a.type == 9 || a.type == 10)
                        { std::memset(buf, 0, sizeof(buf)); strncpy_s(buf, sizeof(buf), a.str.c_str(), _TRUNCATE);
                          const char* hint = a.type == 9 ? "プロパティ名 Prop" : "イベント名 Event";
                          if (pg::InputText(hint, buf, sizeof(buf))) a.str = buf;
                          float v = static_cast<float>(a.num); if (pg::Float("値 Value", &v, 0.05f)) a.num = v; }
                        pg::End();
                    }
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
                if (pg::Begin("Camera"))
                {
                    int projIdx = (cam.projection == CameraProjection::Orthographic) ? 1 : 0;
                    const char* projItems[] = { "Perspective (透視)", "Orthographic (正射)" };
                    if (pg::Combo("投影 Projection", &projIdx, projItems, 2,
                                  "正射は距離で大きさが変わりません（2D/見下ろし向け）。\n"
                                  "近づくと大きくしたいなら透視を選びます。"))
                    {
                        cam.projection = (projIdx == 1) ? CameraProjection::Orthographic
                                                        : CameraProjection::Perspective;
                        changed = true;
                    }

                    // 透視は FOV、正射は Ortho Size（ビュー縦の半分の世界単位）を出す。
                    if (cam.projection == CameraProjection::Perspective)
                        changed |= pg::Float("視野角 FOV", &cam.fovDegrees, 1.0f, 1.0f, 179.0f, "%.1f", &active);
                    else
                        changed |= pg::Float("Ortho Size", &cam.orthoSize, 0.1f, 0.01f, 1000.0f, "%.2f", &active);
                    changed |= pg::Float("Near", &cam.nearClip, 0.01f, 0.001f, 100.0f, "%.3f", &active);
                    changed |= pg::Float("Far", &cam.farClip, 10.0f, 1.0f, 100000.0f, "%.0f", &active);
                    if (pg::Checkbox("アクティブ Active", &cam.isActive))
                    {
                        changed = true;
                        // アクティブカメラは常に1つだけ
                        if (cam.isActive)
                        {
                            for (auto [oe, oc] : reg.view<CameraComponent>().each())
                                if (oe != ctx.selectedEntity) oc.isActive = false;
                        }
                    }
                    pg::End();
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

                if (pg::Begin("RigidBody"))
                {
                    const char* motionTypes[] = { "Static", "Kinematic", "Dynamic" };
                    int motionIdx = static_cast<int>(rb.motionType);
                    if (pg::Combo("挙動 Motion", &motionIdx, motionTypes, 3))
                    {
                        rb.motionType = static_cast<MotionType>(motionIdx);
                        changed = true;
                    }
                    changed |= pg::Float("質量 Mass", &rb.mass, 0.5f, 0.0f, 10000.0f, "%.1f", &active);
                    changed |= pg::Float("摩擦 Friction", &rb.friction, 0.01f, 0.0f, 2.0f, "%.2f", &active);
                    changed |= pg::Float("反発 Bounce", &rb.restitution, 0.01f, 0.0f, 1.0f, "%.2f", &active);
                    changed |= pg::Checkbox("重力 Gravity", &rb.useGravity);
                    pg::End();
                }
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
                if (pg::Begin("BoxCollider"))
                {
                    changed |= pg::Float3("半径 Half Extents", &col.halfExtents.x, 0.05f, 0.01f, 1000.0f, "%.2f", &active);
                    changed |= pg::Float3("オフセット Offset", &col.offset.x, 0.05f, 0, 0, "%.2f", &active);
                    pg::End();
                }
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
                if (pg::Begin("SphereCollider"))
                {
                    changed |= pg::Float("半径 Radius", &col.radius, 0.05f, 0.01f, 1000.0f, "%.2f", &active);
                    changed |= pg::Float3("オフセット Offset", &col.offset.x, 0.05f, 0, 0, "%.2f", &active);
                    pg::End();
                }
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
                if (pg::Begin("CapsuleCollider"))
                {
                    changed |= pg::Float("半径 Radius", &col.radius, 0.05f, 0.01f, 1000.0f, "%.2f", &active);
                    changed |= pg::Float("半分高さ Half Height", &col.halfHeight, 0.05f, 0.01f, 1000.0f, "%.2f", &active);
                    changed |= pg::Float3("オフセット Offset", &col.offset.x, 0.05f, 0, 0, "%.2f", &active);
                    pg::End();
                }
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
                if (pg::Begin("CharacterController"))
                {
                    changed |= pg::Float("半径 Radius",       &cc.radius,       0.02f, 0.05f, 5.0f,    "%.2f", &active);
                    changed |= pg::Float("半分高さ Half Height", &cc.halfHeight, 0.02f, 0.05f, 5.0f,    "%.2f", &active);
                    changed |= pg::Float3("オフセット Offset", &cc.offset.x,     0.02f, 0, 0,           "%.2f", &active);
                    changed |= pg::Float("質量 Mass",         &cc.mass,         1.0f,  1.0f, 1000.0f,  "%.0f", &active);
                    changed |= pg::Float("最大斜度 (deg)",     &cc.maxSlopeDeg,  0.5f,  0.0f, 89.0f,    "%.1f", &active);
                    changed |= pg::Float("段差高さ Step",      &cc.stepHeight,   0.01f, 0.0f, 2.0f,     "%.2f", &active);
                    changed |= pg::Float("ジャンプ速度",       &cc.jumpSpeed,    0.1f,  0.0f, 50.0f,    "%.1f", &active);
                    changed |= pg::Float("重力スケール",       &cc.gravityScale, 0.05f, 0.0f, 5.0f,     "%.2f", &active);
                    pg::End();
                }
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
                if (pg::Begin("ConvexHull"))
                {
                    pg::Text("頂点数 Points", "%d", static_cast<int>(col.points.size()));
                    pg::End();
                }
                ImGui::TextDisabled("(メッシュから自動生成)");
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

                    bool metalActive = false, roughActive = false;
                    bool hasNormal = mat->normalMapTexture != nullptr;
                    bool hasMR2 = mat->metalRoughnessTexture != nullptr;
                    if (pg::Begin("MaterialPBR"))
                    {
                        pg::SliderFloat("金属感 Metallic", &mr.overrideMetallic, 0.0f, 1.0f, "%.3f", &metalActive);
                        pg::SliderFloat("粗さ Roughness", &mr.overrideRoughness, 0.0f, 1.0f, "%.3f", &roughActive);
                        pg::Text("Normal Map", "%s", hasNormal ? "あり" : "なし");
                        pg::Text("MetalRough Map", "%s", hasMR2 ? "あり" : "なし");
                        pg::End();
                    }

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

                    // テクスチャ上書き（アセットブラウザからテクスチャをドラッグ&ドロップして割当。
                    // Unity/Unreal 風）。サブメッシュ単位。Material 自体は同一モデルパスの全インスタンスで
                    // 共有されているため直接書き換えず、MeshRenderer にインスタンス単位で保持する
                    // (描画側 Application::EnsureMaterialOverrideSrv が専用SRVブロックを合成する)。
                    ImGui::Separator();
                    ImGui::TextDisabled("\xe3\x83\x86\xe3\x82\xaf\xe3\x82\xb9\xe3\x83\x81\xe3\x83\xa3\xe4\xb8\x8a\xe6\x9b\xb8\xe3\x81\x8d"
                        "(\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x96\xe3\x83\xa9\xe3\x82\xa6\xe3\x82\xb6\xe3\x81\x8b\xe3\x82\x89 D&D)");

                    // 割当/解除を1箇所にまとめる(ドラッグ&ドロップ・ピッカー・xボタンの全経路から呼ぶ)。
                    auto applyOverride = [&](std::vector<std::string>& slotVec, u32 smi, const std::string& rel)
                    {
                        MeshRenderer before = mr;
                        MeshRenderer::SetOverride(slotVec, smi, rel);
                        ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<MeshRenderer>>(
                            &reg, ctx.selectedEntity, before, mr, "Material Texture"));
                    };

                    constexpr float kThumbSize = 64.0f;

                    auto drawTextureOverrideSlot = [&](const char* label, std::vector<std::string>& slotVec, u32 smi)
                    {
                        ImGui::PushID(label);
                        ImGui::PushID(static_cast<int>(smi));

                        const std::string& cur = MeshRenderer::SafeGetOverride(slotVec, smi);
                        bool hasTex = !cur.empty();

                        pg::Label(label);

                        // サムネイル(実テクスチャ画像)。AssetBrowserPanel と同じキャッシュを使い回す
                        // (GetOrQueueThumbnail、無ければロードキューに積んで0=読込中を返す)。
                        u64 gpuHandle = 0;
                        if (hasTex && m_assetBrowser)
                            gpuHandle = m_assetBrowser->GetOrQueueThumbnail(m_assetsDir + cur);

                        bool clicked;
                        if (gpuHandle != 0)
                        {
                            clicked = ImGui::ImageButton("##slotThumb", static_cast<ImTextureID>(gpuHandle),
                                                          ImVec2(kThumbSize, kThumbSize));
                        }
                        else
                        {
                            clicked = ImGui::Button(hasTex ? "..." : "(default)", ImVec2(kThumbSize * 3.0f, kThumbSize));
                        }
                        if (clicked)
                            ImGui::OpenPopup("TexturePicker");

                        // ドラッグ&ドロップ(アセットブラウザから直接)
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
                            {
                                const char* droppedPath = static_cast<const char*>(payload->Data);
                                namespace fs = std::filesystem;
                                std::string ext = fs::path(droppedPath).extension().string();
                                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                                if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Texture)
                                {
                                    std::string abs = fs::path(droppedPath).lexically_normal().string();
                                    std::string base = fs::path(m_assetsDir).lexically_normal().string();
                                    std::replace(abs.begin(), abs.end(), '\\', '/');
                                    std::replace(base.begin(), base.end(), '\\', '/');
                                    std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
                                    applyOverride(slotVec, smi, rel);
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }

                        if (hasTex)
                        {
                            ImGui::SameLine();
                            ImGui::BeginGroup();
                            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 160.0f);
                            ImGui::TextWrapped("%s", cur.c_str());
                            ImGui::PopTextWrapPos();
                            if (ImGui::SmallButton("x"))
                                applyOverride(slotVec, smi, "");
                            ImGui::EndGroup();
                        }

                        // クリックで開く一覧ダイアログ(Unity風: プロジェクト内の全テクスチャから選択)
                        if (ImGui::BeginPopup("TexturePicker"))
                        {
                            ImGui::TextDisabled("Select Texture");
                            ImGui::Separator();
                            if (ImGui::Selectable("(None)", !hasTex))
                            {
                                applyOverride(slotVec, smi, "");
                                ImGui::CloseCurrentPopup();
                            }

                            namespace fs = std::filesystem;
                            std::error_code ec;
                            fs::path root(m_assetsDir);
                            if (fs::exists(root, ec))
                            {
                                fs::recursive_directory_iterator dirIt(
                                    root, fs::directory_options::skip_permission_denied, ec);
                                fs::recursive_directory_iterator dirEnd;
                                for (; !ec && dirIt != dirEnd; dirIt.increment(ec))
                                {
                                    std::error_code fec;
                                    if (!dirIt->is_regular_file(fec) || fec) continue;
                                    std::string rowExt = dirIt->path().extension().string();
                                    for (char& c : rowExt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                    if (AssetBrowserPanel::ClassifyExtension(rowExt) != AssetBrowserPanel::AssetType::Texture)
                                        continue;

                                    fs::path relPath = fs::relative(dirIt->path(), root, fec);
                                    if (fec) continue;
                                    std::string relStr = relPath.generic_string();

                                    ImGui::PushID(relStr.c_str());
                                    u64 rowThumb = m_assetBrowser
                                        ? m_assetBrowser->GetOrQueueThumbnail(dirIt->path().string()) : 0;
                                    if (rowThumb != 0)
                                    {
                                        ImGui::Image(static_cast<ImTextureID>(rowThumb), ImVec2(20.0f, 20.0f));
                                        ImGui::SameLine();
                                    }
                                    if (ImGui::Selectable(relStr.c_str(), cur == relStr))
                                    {
                                        applyOverride(slotVec, smi, relStr);
                                        ImGui::CloseCurrentPopup();
                                    }
                                    ImGui::PopID();
                                }
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::PopID();
                        ImGui::PopID();
                    };

                    // マテリアルアセット(assets/materials/*.dxmat、Unrealのマテリアルインスタンス相当)。
                    // 割当があれば上記3スロットのテクスチャ個別上書きより優先される(描画側 Application 参照)。
                    auto drawMaterialAssetSlot = [&](u32 smi)
                    {
                        ImGui::PushID("MaterialAsset");
                        ImGui::PushID(static_cast<int>(smi));

                        const std::string& cur = MeshRenderer::SafeGetOverride(mr.materialAsset, smi);
                        bool hasMat = !cur.empty();

                        pg::Label("Material Asset");

                        // サムネイルは .dxmat の albedo テクスチャを軽量パースして使い回す
                        // (AssetBrowserPanel::Refresh と同じ方式。JSON数百バイトなので毎フレーム読んでも軽い)。
                        u64 gpuHandle = 0;
                        if (hasMat && m_assetBrowser)
                        {
                            std::ifstream ifs(m_assetsDir + cur, std::ios::binary);
                            if (ifs)
                            {
                                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                                MaterialAssetData data;
                                if (ParseMaterialAsset(bytes, data) && !data.albedoPath.empty())
                                    gpuHandle = m_assetBrowser->GetOrQueueThumbnail(m_assetsDir + data.albedoPath);
                            }
                        }

                        bool clicked;
                        if (gpuHandle != 0)
                        {
                            clicked = ImGui::ImageButton("##matThumb", static_cast<ImTextureID>(gpuHandle),
                                                          ImVec2(kThumbSize, kThumbSize));
                        }
                        else
                        {
                            clicked = ImGui::Button(hasMat ? "..." : "(none)", ImVec2(kThumbSize * 3.0f, kThumbSize));
                        }
                        if (clicked)
                            ImGui::OpenPopup("MaterialPicker");

                        // ドラッグ&ドロップ(アセットブラウザから .dxmat のみ受理)
                        if (ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
                            {
                                const char* droppedPath = static_cast<const char*>(payload->Data);
                                namespace fs = std::filesystem;
                                std::string ext = fs::path(droppedPath).extension().string();
                                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                                if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Material)
                                {
                                    std::string abs = fs::path(droppedPath).lexically_normal().string();
                                    std::string base = fs::path(m_assetsDir).lexically_normal().string();
                                    std::replace(abs.begin(), abs.end(), '\\', '/');
                                    std::replace(base.begin(), base.end(), '\\', '/');
                                    std::string rel = (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
                                    applyOverride(mr.materialAsset, smi, rel);

                                    // 割当時: UVタイリングが既定値(1.0)のままなら.dxmatの値を初期値としてコピーする
                                    // (描画時に合成はしない。Mesh::ApplyUVScaleは頂点焼き込みのため)。
                                    if (mr.uvScaleU == 1.0f && mr.uvScaleV == 1.0f)
                                    {
                                        std::ifstream ifs2(m_assetsDir + rel, std::ios::binary);
                                        if (ifs2)
                                        {
                                            std::vector<uint8_t> bytes2((std::istreambuf_iterator<char>(ifs2)),
                                                                         std::istreambuf_iterator<char>());
                                            MaterialAssetData data2;
                                            if (ParseMaterialAsset(bytes2, data2) && mr.meshes[smi])
                                            {
                                                mr.uvScaleU = data2.uvTilingU;
                                                mr.uvScaleV = data2.uvTilingV;
                                                if (scene)
                                                    for (auto* mesh : mr.meshes)
                                                        if (mesh) mesh->ApplyUVScale(*scene->GetDevice(), mr.uvScaleU, mr.uvScaleV);
                                            }
                                        }
                                    }
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }

                        if (hasMat)
                        {
                            ImGui::SameLine();
                            ImGui::BeginGroup();
                            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 160.0f);
                            ImGui::TextWrapped("%s", cur.c_str());
                            ImGui::PopTextWrapPos();
                            if (ImGui::SmallButton("x"))
                                applyOverride(mr.materialAsset, smi, "");
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Edit"))
                            {
                                ctx.pendingOpenMaterialPath = m_assetsDir + cur;
                                ctx.showMaterialEditor = true;
                            }
                            ImGui::EndGroup();
                        }

                        // クリックで開く一覧ダイアログ(TexturePickerの.dxmat版)
                        if (ImGui::BeginPopup("MaterialPicker"))
                        {
                            ImGui::TextDisabled("Select Material");
                            ImGui::Separator();
                            if (ImGui::Selectable("(None)", !hasMat))
                            {
                                applyOverride(mr.materialAsset, smi, "");
                                ImGui::CloseCurrentPopup();
                            }

                            namespace fs = std::filesystem;
                            std::error_code ec;
                            fs::path root(m_assetsDir);
                            if (fs::exists(root, ec))
                            {
                                fs::recursive_directory_iterator dirIt(
                                    root, fs::directory_options::skip_permission_denied, ec);
                                fs::recursive_directory_iterator dirEnd;
                                for (; !ec && dirIt != dirEnd; dirIt.increment(ec))
                                {
                                    std::error_code fec;
                                    if (!dirIt->is_regular_file(fec) || fec) continue;
                                    std::string rowExt = dirIt->path().extension().string();
                                    for (char& c : rowExt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                                    if (AssetBrowserPanel::ClassifyExtension(rowExt) != AssetBrowserPanel::AssetType::Material)
                                        continue;

                                    fs::path relPath = fs::relative(dirIt->path(), root, fec);
                                    if (fec) continue;
                                    std::string relStr = relPath.generic_string();

                                    if (ImGui::Selectable(relStr.c_str(), cur == relStr))
                                    {
                                        applyOverride(mr.materialAsset, smi, relStr);
                                        ImGui::CloseCurrentPopup();
                                    }
                                }
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::PopID();
                        ImGui::PopID();
                    };

                    if (pg::Begin("MaterialTex"))
                    {
                        for (u32 smi = 0; smi < static_cast<u32>(mr.meshes.size()); ++smi)
                        {
                            if (!mr.meshes[smi]) continue;
                            if (mr.meshes.size() > 1)
                            {
                                char sub[32];
                                snprintf(sub, sizeof(sub), "Submesh %u", smi);
                                pg::Group(sub);
                            }
                            drawMaterialAssetSlot(smi);

                            bool matAssigned = mr.HasMaterialAsset(smi);
                            if (matAssigned)
                            {
                                ImGui::BeginDisabled();
                                ImGui::TextDisabled("\xe3\x83\x9e\xe3\x83\x86\xe3\x83\xaa\xe3\x82\xa2\xe3\x83\xab\xe5\x89\xb2\xe5\xbd\x93\xe4\xb8\xad\xe3\x81\xaf\xe7\x84\xa1\xe5\x8a\xb9"); // マテリアル割当中は無効
                            }
                            drawTextureOverrideSlot("Albedo", mr.overrideAlbedoTexture, smi);
                            drawTextureOverrideSlot("Normal", mr.overrideNormalTexture, smi);
                            drawTextureOverrideSlot("MetalRoughness", mr.overrideMetalRoughnessTexture, smi);
                            if (matAssigned)
                                ImGui::EndDisabled();
                        }
                        pg::End();
                    }
                }
            }

            // UV タイリング
            if (IconHeader(nullptr, 0, "UV Tiling"))
            {
                bool uvChanged = false;
                if (pg::Begin("UVTiling"))
                {
                    uvChanged |= pg::Float("U Scale", &mr.uvScaleU, 0.1f, 0.01f, 100.0f, "%.2f");
                    uvChanged |= pg::Float("V Scale", &mr.uvScaleV, 0.1f, 0.01f, 100.0f, "%.2f");
                    pg::End();
                }
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

            // カスタムシェーダー割当（静的メッシュのみ有効。スキンド/インスタンシングは既定へフォールバック）。
            // 一覧はプロジェクト assets/shaders/ 配下の .hlsl のうち、Registry(エンジン組み込み)と
            // 一致しないもの＝自作シェーダーだけ(一致するものは全体に効く「上書き」用途なので個別割当から除外)。
            if (IconHeader(nullptr, 0, "Shader"))
            {
                namespace fs = std::filesystem;
                std::string currentLabel = mr.shaderPath.empty() ? "\xe6\x97\xa2\xe5\xae\x9a (Forward)" : mr.shaderPath;
                bool shaderGrid = pg::Begin("MeshShader");
                if (shaderGrid)
                    pg::Label("\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x80\xe3\x83\xbc");  // シェーダー
                if (ImGui::BeginCombo("##meshShader", currentLabel.c_str()))
                {
                    std::vector<std::string> options;
                    std::error_code ec;
                    fs::path root(m_assetsDir + "shaders/");
                    if (fs::exists(root, ec))
                    {
                        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
                        fs::recursive_directory_iterator end;
                        for (; !ec && it != end; it.increment(ec))
                        {
                            std::error_code fec;
                            if (!it->is_regular_file(fec) || fec) continue;
                            if (it->path().extension() != L".hlsl") continue;
                            fs::path rel = fs::relative(it->path(), root, fec);
                            if (fec) continue;
                            std::string relStr = rel.generic_string();
                            if (FindShaderSourceByRelPath(relStr) != nullptr)
                                continue;  // Registry一致=上書き用途。個別割当の選択肢からは除外
                            options.push_back(relStr);
                        }
                    }

                    if (ImGui::Selectable("\xe6\x97\xa2\xe5\xae\x9a (Forward)", mr.shaderPath.empty()))
                        mr.shaderPath.clear();
                    for (const auto& opt : options)
                    {
                        if (ImGui::Selectable(opt.c_str(), mr.shaderPath == opt))
                            mr.shaderPath = opt;
                    }
                    if (options.empty())
                        ImGui::TextDisabled("(assets/shaders/ \xe3\x81\xab\xe8\x87\xaa\xe4\xbd\x9c\xe3\x82\xb7\xe3\x82\xa7\xe3\x83\xbc\xe3\x83\x80\xe3\x83\xbc\xe3\x81\xaa\xe3\x81\x97)");
                    ImGui::EndCombo();
                }
                // カスタムシェーダー割当時のみ意味を持つ: PSが書く float4 の alpha を実際に
                // ブレンドに使うかどうか。既定 OFF(不透明、DepthWrite=ON)のままだと
                // シェーダー側でどれだけ alpha を作り込んでも画面には反映されない。
                if (shaderGrid)
                {
                    if (!mr.shaderPath.empty())
                    {
                        pg::Checkbox("アルファブレンド有効", &mr.shaderAlphaBlend,
                            "ON: シェーダーの alpha 出力を SrcAlpha/InvSrcAlpha でブレンド(DepthWrite OFF)。"
                            "OFF(既定): 不透明固定で alpha は無視される");
                        pg::Float("エフェクト値 effectValue", &mr.effectValue, 0.005f, 0.0f, 1.0f, "%.3f", nullptr,
                            "シェーダーへ渡す汎用の進捗/強度値(意味はシェーダー依存)。"
                            "Luaの scene:setMeshEffect(e, value) で実行時にも変更可");
                        pg::Float4("パラメーター shaderParams", &mr.shaderParams.x, 0.01f, 0.0f, 0.0f, "%.3f", nullptr,
                            "シェーダーへ渡す汎用パラメーター4つ(意味はシェーダー依存)。HLSL側は cbuffer の "
                            "effectValue の後ろに float4 shaderParams; を足して読む。"
                            "Luaの scene:setMeshParams(e, x,y,z,w) でも変更可");
                    }
                    pg::End();
                }
                if (reg.all_of<SkeletalAnimation>(ctx.selectedEntity) && !mr.shaderPath.empty())
                    ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                        "スキンドメッシュは既定シェーダーへフォールバック");
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
            AddComponentMenuItem<TrailRenderer>(reg, ctx, ctx.selectedEntity, "Trail Renderer");
            AddComponentMenuItem<Trigger>(reg, ctx, ctx.selectedEntity, "Trigger");
            ImGui::Separator();
            AddComponentMenuItem<UICanvas>(reg, ctx, ctx.selectedEntity, "UI Canvas");
            AddComponentMenuItem<UIRect>(reg, ctx, ctx.selectedEntity, "UI Rect");
            AddComponentMenuItem<UIImage>(reg, ctx, ctx.selectedEntity, "UI Image");
            AddComponentMenuItem<UIText>(reg, ctx, ctx.selectedEntity, "UI Text");
            AddComponentMenuItem<UIButton>(reg, ctx, ctx.selectedEntity, "UI Button");
            AddComponentMenuItem<UIAnimator>(reg, ctx, ctx.selectedEntity, "UI Animator");
            ImGui::Separator();
            AddComponentMenuItem<RigidBody>(reg, ctx, ctx.selectedEntity, "RigidBody");
            AddComponentMenuItem<BoxCollider>(reg, ctx, ctx.selectedEntity, "Box Collider");
            AddComponentMenuItem<SphereCollider>(reg, ctx, ctx.selectedEntity, "Sphere Collider");
            AddComponentMenuItem<CapsuleCollider>(reg, ctx, ctx.selectedEntity, "Capsule Collider");
            AddComponentMenuItem<CharacterController>(reg, ctx, ctx.selectedEntity, "Character Controller");
            ImGui::Separator();
            AddComponentMenuItem<NetworkIdentity>(reg, ctx, ctx.selectedEntity, "Network Identity");
            AddComponentMenuItem<NetworkTransform>(reg, ctx, ctx.selectedEntity, "Network Transform");
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

    // Inspector ウィンドウ全体を .lua とテクスチャのドロップ先にする（どこにドロップしても付く）。
    // 個別スロット(テクスチャ上書きUI等)の上では小さいターゲットが優先される(ImGui は面積最小を採用)。
    if (ctx.HasSelection())
    {
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (win && ImGui::BeginDragDropTargetCustom(win->Rect(), win->ID))
        {
            namespace fs = std::filesystem;
            // ドロップパスを assets 相対へ（.lua/テクスチャ共通）
            auto toAssetsRel = [this](const char* pathCStr) {
                auto abs  = fs::path(pathCStr).lexically_normal().string();
                auto base = fs::path(m_assetsDir).lexically_normal().string();
                std::replace(abs.begin(),  abs.end(),  '\\', '/');
                std::replace(base.begin(), base.end(), '\\', '/');
                return (abs.rfind(base, 0) == 0) ? abs.substr(base.size()) : abs;
            };

            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DND_SCRIPT"))
            {
                std::string rel = toAssetsRel(static_cast<const char*>(payload->Data));
                for (auto ent : ctx.selectedEntities)
                    ctx.pendingScriptAttachments.push_back({ent, rel});
            }

            // テクスチャ: 選択エンティティの Albedo に割当（全サブメッシュ、Undo対応）。
            // スロットに正確に落とさなくても「オブジェクトを選んで Inspector に投げる」だけで貼れる。
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload(AssetBrowserPanel::kDragDropPayloadType))
            {
                const char* pathCStr = static_cast<const char*>(payload->Data);
                std::string ext = fs::path(pathCStr).extension().string();
                for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (AssetBrowserPanel::ClassifyExtension(ext) == AssetBrowserPanel::AssetType::Texture)
                {
                    std::string rel = toAssetsRel(pathCStr);
                    for (auto ent : ctx.selectedEntities)
                    {
                        auto* mr = reg.try_get<MeshRenderer>(ent);
                        if (!mr) continue;
                        MeshRenderer before = *mr;
                        const u32 n = mr->meshes.empty() ? 1u : static_cast<u32>(mr->meshes.size());
                        for (u32 smi = 0; smi < n; ++smi)
                            MeshRenderer::SetOverride(mr->overrideAlbedoTexture, smi, rel);
                        ctx.undoSystem.PushCommand(std::make_unique<ComponentEditCommand<MeshRenderer>>(
                            &reg, ent, before, *mr, "Material Texture"));
                    }
                }
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
        if (pg::Begin("PointLight"))
        {
            changed |= pg::Color3("色 Color", &pl.color.x, ImGuiColorEditFlags_NoInputs);
            active  |= ImGui::IsItemActive();
            changed |= pg::SliderFloat("明るさ Brightness", &pl.intensity, 0.0f, 20.0f, "%.2f", &active);
            changed |= pg::SliderFloat("距離 Range", &pl.range, 0.1f, 100.0f, "%.1f", &active,
                "位置は Transform で決まります");
            changed |= pg::Checkbox("影を落とす Shadows", &pl.castShadows,
                "同時に影を落とせるポイントライトは最大2灯（カメラに近い順で優先）。\n"
                "超えた分は影なしにフォールバックします。");
            pg::End();
        }
        EndEdit(reg, ctx, e, m_plEdit, changed, active, "PointLight");
    }
    if (reg.all_of<DirectionalLight>(e))
    {
        BeginEdit(reg, e, m_dlEdit);
        auto& dl = reg.get<DirectionalLight>(e);
        bool changed = false, active = false;
        previewSwatch(dl.color, dl.intensity);
        if (pg::Begin("DirectionalLight"))
        {
            changed |= pg::Color3("色 Color", &dl.color.x, ImGuiColorEditFlags_NoInputs);
            active  |= ImGui::IsItemActive();
            changed |= pg::SliderFloat("明るさ Brightness", &dl.intensity, 0.0f, 10.0f, "%.2f", &active);
            changed |= pg::SliderFloat("環境光 Ambient", &dl.ambient, 0.0f, 1.0f, "%.2f", &active);
            pg::End();
        }
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
        if (pg::Begin("SpotLight"))
        {
            changed |= pg::Color3("色 Color", &sl.color.x, ImGuiColorEditFlags_NoInputs);
            active  |= ImGui::IsItemActive();
            changed |= pg::SliderFloat("明るさ Brightness", &sl.intensity, 0.0f, 30.0f, "%.2f", &active);
            changed |= pg::SliderFloat("距離 Range", &sl.range, 0.1f, 100.0f, "%.1f", &active);
            changed |= pg::SliderFloat("内側コーン Inner", &sl.innerConeDeg, 1.0f, 80.0f, "%.0f°", &active);
            changed |= pg::SliderFloat("外側コーン Outer", &sl.outerConeDeg, 1.0f, 89.0f, "%.0f°", &active);
            if (sl.innerConeDeg > sl.outerConeDeg) sl.innerConeDeg = sl.outerConeDeg;
            changed |= pg::Checkbox("影を落とす Shadows", &sl.castShadows,
                "同時に影を落とせるスポットライトは最大4灯（カメラに近い順で優先）。\n"
                "超えた分は影なしにフォールバックします。");
            pg::End();
        }
        changed |= DirectionEditor("slDir", sl.direction, active);
        ImGui::TextDisabled("位置は Transform、向きは上の方向で決まります");
        EndEdit(reg, ctx, e, m_slEdit, changed, active, "SpotLight");
    }

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

    BeginEdit(reg, e, m_audioEdit);
    bool changed = false, active = false;

    if (pg::Begin("AudioSource"))
    {
        char buf[256] = {};
        size_t n = as.clipPath.copy(buf, sizeof(buf) - 1);
        buf[n] = '\0';
        if (pg::InputText("クリップ Clip", buf, sizeof(buf), 0, &active))
        { as.clipPath = buf; changed = true; }

        changed |= pg::SliderFloat("音量 Volume", &as.volume, 0.0f, 1.0f, "%.2f", &active);

        pg::Group("再生");
        changed |= pg::Checkbox("開始時に再生 Play On Start", &as.playOnStart);
        changed |= pg::Checkbox("ループ Loop", &as.loop);

        pg::Group("空間化");
        changed |= pg::Checkbox("3D 空間音にする Spatial", &as.spatial,
            "空間化はモノラル wav のみ。位置は Transform。");
        if (as.spatial)
        {
            changed |= pg::Float("最小距離 Min", &as.minDistance, 0.1f, 0.0f, 1000.0f, "%.1f", &active);
            changed |= pg::Float("最大距離 Max", &as.maxDistance, 0.5f, 0.1f, 5000.0f, "%.1f", &active);
        }
        pg::End();
    }

    EndEdit(reg, ctx, e, m_audioEdit, changed, active, "Audio Source");
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
                                          GameClock* /*clock*/)  // ビルド節を移設して未使用に
{
    ImGui::Begin("\xe3\x82\xa8\xe3\x83\xb3\xe3\x82\xb8\xe3\x83\xb3\xe8\xa8\xad\xe5\xae\x9a");  // エンジン設定

    // --- Camera ---
    if (IconHeader(ctx.icons, ctx.icons ? ctx.icons->entCamera : 0,
                   "\xe3\x82\xab\xe3\x83\xa1\xe3\x83\xa9", ImGuiTreeNodeFlags_DefaultOpen))  // Camera
    {
        if (pg::Begin("EngineCam"))
        {
            auto camPos = camera->GetPosition();
            pg::Text("位置", "%.1f, %.1f, %.1f", camPos.x, camPos.y, camPos.z);
            f32 moveSpeed = camera->GetMoveSpeed();
            if (pg::SliderFloat("移動速度", &moveSpeed, 1.0f, 50.0f, "%.1f"))
                camera->SetMoveSpeed(moveSpeed);
            pg::End();
        }
    }

    // --- Shadow quality ---
    if (IconHeader(nullptr, 0, "シャドウ"))
    {
        const char* qualities[] = {"1024 (Low)", "2048 (Medium)", "4096 (High)", "8192 (Ultra)"};
        const u32 sizes[] = {1024, 2048, 4096, 8192};
        if (pg::Begin("EngineShadow"))
        {
            if (pg::Combo("解像度", &shadowQualityIndex, qualities, 4))
            {
                shadowMapSize = sizes[shadowQualityIndex];
                shadowMapDirty = true;
            }
            pg::Text("実サイズ", "%ux%u", shadowMapSize, shadowMapSize);

            pg::Group("CSM (4分割)");
            pg::SliderFloat("分割λ", &cascadeSplitLambda, 0.0f, 1.0f, "%.2f");
            pg::SliderFloat("境界ブレンド", &cascadeBlendBand, 0.0f, 5.0f, "%.2f");
            pg::Checkbox("カスケード可視化", &showCascadeDebug);
            pg::End();
        }
    }

    // --- Audio ---
    if (IconHeader(ctx.icons, ctx.icons ? ctx.icons->entAudio : 0,
                   "\xe3\x82\xaa\xe3\x83\xbc\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xaa"))  // Audio
    {
        f32 masterVol = audioSystem->GetMasterVolume();
        f32 bgmVol    = audioSystem->GetBGMVolume();
        f32 sfxVol    = audioSystem->GetSFXVolume();
        if (pg::Begin("EngineAudio"))
        {
            if (pg::SliderFloat("マスター", &masterVol, 0.0f, 1.0f, "%.2f"))
                audioSystem->SetMasterVolume(masterVol);
            if (pg::SliderFloat("BGM", &bgmVol, 0.0f, 1.0f, "%.2f"))
                audioSystem->SetBGMVolume(bgmVol);
            if (pg::SliderFloat("SE", &sfxVol, 0.0f, 1.0f, "%.2f"))
                audioSystem->SetSFXVolume(sfxVol);
            pg::End();
        }

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
    if (IconHeader(nullptr, 0, "設定"))
    {
        if (pg::Begin("EngineMisc"))
        {
            pg::Checkbox("VSync", &useVsync);

            bool debugDraw = physicsDebugRenderer->IsEnabled();
            if (pg::Checkbox("Physics Debug", &debugDraw))
            {
                physicsDebugRenderer->SetEnabled(debugDraw);
                physicsDebugDraw = debugDraw;
            }
            pg::End();
        }
    }

    // --- Build ---
    if (IconHeader(ctx.icons, ctx.icons ? ctx.icons->build : 0,
                   "\xe3\x83\x93\xe3\x83\xab\xe3\x83\x89"))  // Build
    {
        // ビルドは専用の「ビルド設定」パネルで構成・開始シーン・出力先を決めてから実行する。
        if (ImGui::Button("ビルド設定を開く"))
            ctx.showBuildSettings = true;
    }

    ImGui::End();
}

} // namespace dx12e
