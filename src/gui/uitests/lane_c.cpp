// UI テストのレーン C の本体。宣言は gui/UiTestHarnessShared.h の lane-C:decl、
// 登録は gui/UiTestHarness.cpp の kTests の lane-C ブロックに足す。
#include "gui/UiTestHarnessShared.h"

#include "core/Application.h"
#include "core/PathResolver.h"
#include "ecs/Components.h"
#include "editor/EditorCommands.h"
#include "editor/EditorContext.h"
#include "renderer/Mesh.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#pragma warning(push)
#pragma warning(disable: 4100 4189 4201 4244 4267 4996)
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_te_context.h>
#include <imgui_te_engine.h>
#pragma warning(pop)

namespace dx12e
{
namespace uitest
{

namespace
{
bool HierarchyRowDrawn(ImGuiTestContext* ctx, entt::entity e);

// 生成要求を積んで（人の操作と同じ経路）、選択が「新しいエンティティ 1 件」になるか確かめる。
void ExpectSelectsNewEntity(ImGuiTestContext* ctx, EditorContext* ed, Scene* scene,
                            const char* what, void (*spawn)(ImGuiTestContext*, EditorContext*))
{
    LogStep(ctx, "W05-21: %s", what);
    auto& reg = scene->GetRegistry();
    const size_t before = reg.storage<NameTag>().size();
    ed->ClearSelection();
    spawn(ctx, ed);
    ctx->Yield(8);   // 生成はフレーム境界の遅延処理
    IM_CHECK_EQ(reg.storage<NameTag>().size(), before + 1);
    IM_CHECK_EQ(ed->selectedEntities.size(), (size_t)1);
    IM_CHECK(ed->selectedEntity != entt::null);
    IM_CHECK(reg.valid(ed->selectedEntity));
    IM_CHECK(ed->selectedEntities.size() == 1 && ed->selectedEntities[0] == ed->selectedEntity);
    IM_CHECK(!ed->revealInHierarchy);   // ヒエラルキーが露出を処理した
    ctx->Yield(4);
    IM_CHECK(HierarchyRowDrawn(ctx, ed->selectedEntity));   // 行が実際に描かれている
}

// 三角形 1 枚だけの最小 .glb を書き出す（外部のモデルに頼らず .glb 分岐を通すため）
bool WriteTinyGlb(const std::string& path)
{
    std::string json =
        "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36,\"target\":34962}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[0,0,0],\"max\":[1,1,0]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    while (json.size() % 4) json.push_back(' ');
    const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    auto put = [](std::string& o, uint32_t v) { o.append(reinterpret_cast<const char*>(&v), 4); };
    std::string out;
    put(out, 0x46546C67); put(out, 2);
    put(out, static_cast<uint32_t>(12 + 8 + json.size() + 8 + sizeof(pos)));
    put(out, static_cast<uint32_t>(json.size())); put(out, 0x4E4F534A); out += json;
    put(out, static_cast<uint32_t>(sizeof(pos))); put(out, 0x004E4942);
    out.append(reinterpret_cast<const char*>(pos), sizeof(pos));
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) return false;
    ofs.write(out.data(), static_cast<std::streamsize>(out.size()));
    return ofs.good();
}

// 1 エンティティのプレハブを assets に書き出す（書き出したあと元のエンティティは消す）
bool WriteTinyPrefab(Scene* scene, const std::string& assets, const std::string& rel, const char* name)
{
    auto& reg = scene->GetRegistry();
    auto src = reg.create();
    reg.emplace<NameTag>(src, NameTag{name});
    reg.emplace<Transform>(src, Transform{});
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(assets + rel).parent_path(), ec);
    const bool ok = SceneSerializer::SavePrefab(*scene, src, assets + rel, assets);
    reg.destroy(src);
    return ok;
}

// ヒエラルキーの行（PushID(entity) → TreeNode "##node"）が今描かれているか。
bool HierarchyRowDrawn(ImGuiTestContext* ctx, entt::entity e)
{
    ImGuiWindow* w = ImGui::FindWindowByName(kWinHierarchyName);
    if (!w) return false;
    const int ei = static_cast<int>(static_cast<uint32_t>(e));
    const ImGuiID base = ImHashData(&ei, sizeof(int), w->ID);
    const ImGuiID id = ImHashStr("##node", 0, base);
    return ctx->ItemInfo(id, ImGuiTestOpFlags_NoError).ID != 0;
}

// MCP 由来（client!=0）の生成要求では選択が変わらない
void ExpectMcpKeepsSelection(ImGuiTestContext* ctx, EditorContext* ed, Scene* scene,
                             entt::entity sentinel, const char* marker, const char* what)
{
    LogStep(ctx, "W05-21: MCP の生成は選択を変えない: %s", what);
    auto& reg = scene->GetRegistry();
    ed->ClearSelection();
    ed->Select(sentinel);
    const size_t before = reg.storage<NameTag>().size();
    PendingSpawnRequest r;
    r.modelPath = marker;
    r.mcp.client = 0x7FFFFFF1u;   // 実在しない接続（応答は捨てられる）
    r.mcp.requestId = 1;
    r.mcp.method = "create_entity";
    ed->pendingSpawns.push_back(r);
    ctx->Yield(8);
    IM_CHECK_GT(reg.storage<NameTag>().size(), before);   // 実際に生成された
    IM_CHECK_EQ(ed->selectedEntities.size(), (size_t)1);
    IM_CHECK(ed->selectedEntity == sentinel);
}
} // namespace

// W05-21: 人の操作の生成経路で新しいエンティティが選択される。MCP の生成では選択は変わらない。
// Undo で選択が無効 id を指さない。
void T_CreateEntitySelects(ImGuiTestContext* ctx)
{
    EditorContext* ed = Editor();
    Application*   app = App();
    if (!ed || !app || !app->GetScene()) { IM_ERRORF("Application / Scene を取得できません"); return; }
    Scene* scene = app->GetScene();
    auto& reg = scene->GetRegistry();
    const std::string assets = PathResolver::AssetsDir();
    std::error_code rmEc;

    // 経路 1: ヒエラルキーの「＋エンティティ追加」
    ExpectSelectsNewEntity(ctx, ed, scene, "ヒエラルキーの＋で Box",
        [](ImGuiTestContext* c, EditorContext*) { AddEntity(c, "Box"); });

    // 経路 2: Ctrl+K（コマンドパレット）が呼ぶ SpawnItem そのもの
    ExpectSelectsNewEntity(ctx, ed, scene, "SpawnItem（Ctrl+K の実体。Sphere）",
        [](ImGuiTestContext*, EditorContext* e) {
            for (const auto& it : cmd::kCreateItems)
                if (std::strcmp(it.id, "create.sphere") == 0) { cmd::SpawnItem(*e, it); return; }
        });

    // 経路 3: .glb の配置（SpawnItem → Scene::Spawn の分岐）
    IM_CHECK(WriteTinyGlb(assets + "__uitest_w0521.glb"));
    ExpectSelectsNewEntity(ctx, ed, scene, "実 .glb の配置",
        [](ImGuiTestContext*, EditorContext* e) {
            cmd::CreateItem it{"uitest.glb", "glb", "glb", "__uitest_w0521.glb", 0.0f, "基本", ""};
            cmd::SpawnItem(*e, it);
        });
    std::filesystem::remove(assets + "__uitest_w0521.glb", rmEc);

    // 経路 4: プレハブ（ルートが選択される）
    IM_CHECK(WriteTinyPrefab(scene, assets, "prefabs/__uitest_w0521.prefab", "__uitest_w0521_src"));
    ExpectSelectsNewEntity(ctx, ed, scene, "プレハブの配置",
        [](ImGuiTestContext*, EditorContext* e) {
            PendingSpawnRequest r; r.modelPath = "prefabs/__uitest_w0521.prefab";
            e->pendingSpawns.push_back(r);
        });
    std::filesystem::remove(assets + "prefabs/__uitest_w0521.prefab", rmEc);

    // 人の操作で作った UI 要素: Canvas の下に作られ、その行がヒエラルキーに出る（親が開く）
    {
        LogStep(ctx, "W05-21: UI 要素の行がヒエラルキーに出る");
        ed->ClearSelection();
        PendingSpawnRequest cv; cv.modelPath = "__ui_canvas__";
        ed->pendingSpawns.push_back(cv);
        ctx->Yield(8);
        const entt::entity canvas = ed->selectedEntity;
        IM_CHECK(canvas != entt::null && reg.valid(canvas) && reg.all_of<UICanvas>(canvas));
        ed->ClearSelection();
        PendingSpawnRequest im; im.modelPath = "__ui_image__";
        ed->pendingSpawns.push_back(im);
        ctx->Yield(8);
        const entt::entity img = ed->selectedEntity;
        IM_CHECK(img != entt::null && reg.valid(img) && reg.all_of<UIImage>(img));
        IM_CHECK(reg.get<Transform>(img).parent != entt::null);
        ctx->Yield(4);
        IM_CHECK(HierarchyRowDrawn(ctx, img));

        // 続けて作っても入れ子の鎖にならない（選択が Image のとき、Text は Image の兄弟 = Canvas 直下）
        PendingSpawnRequest tx; tx.modelPath = "__ui_text__";
        ed->pendingSpawns.push_back(tx);
        ctx->Yield(8);
        const entt::entity txt = ed->selectedEntity;
        IM_CHECK(txt != entt::null && reg.valid(txt) && txt != img);
        IM_CHECK(reg.get<Transform>(txt).parent == canvas);
        PendingSpawnRequest bt; bt.modelPath = "__ui_button__";
        ed->pendingSpawns.push_back(bt);
        ctx->Yield(8);
        const entt::entity btn = ed->selectedEntity;
        IM_CHECK(btn != entt::null && reg.valid(btn) && btn != txt);
        IM_CHECK(reg.get<Transform>(btn).parent == canvas);
        // Canvas を選んで作れば Canvas の子
        ed->ClearSelection(); ed->Select(canvas);
        PendingSpawnRequest sl; sl.modelPath = "__ui_slider__";
        ed->pendingSpawns.push_back(sl);
        ctx->Yield(8);
        IM_CHECK(reg.get<Transform>(ed->selectedEntity).parent == canvas);
        // ScrollView（入れ物）を選んで作れば ScrollView の子
        ed->ClearSelection(); ed->Select(canvas);
        PendingSpawnRequest sv; sv.modelPath = "__ui_scrollview__";
        ed->pendingSpawns.push_back(sv);
        ctx->Yield(8);
        const entt::entity scroll = ed->selectedEntity;
        IM_CHECK(scroll != entt::null && reg.valid(scroll) && reg.all_of<UIScrollView>(scroll));
        PendingSpawnRequest tx2; tx2.modelPath = "__ui_text__";
        ed->pendingSpawns.push_back(tx2);
        ctx->Yield(8);
        IM_CHECK(reg.get<Transform>(ed->selectedEntity).parent == scroll);
    }

    // MCP の生成は選択を変えない（primitive / ui_button（ラベル子つき） / prefab）
    auto firstEntity = [&]() {
        entt::entity f = entt::null;
        for (auto [e, n] : reg.view<NameTag>().each()) { f = e; break; }
        return f;
    };
    entt::entity sentinel = firstEntity();
    IM_CHECK(sentinel != entt::null);
    ExpectMcpKeepsSelection(ctx, ed, scene, sentinel, "__primitive_box__", "primitive");
    ExpectMcpKeepsSelection(ctx, ed, scene, sentinel, "__ui_button__", "ui_button");
    {
        // C14 / R1-14: 既定の放出器にはレイヤーが 1 枚入っている（空だと「何も出さない」ので）
        ExpectMcpKeepsSelection(ctx, ed, scene, sentinel, "__particle_emitter__", "particle_emitter");
        bool found = false;
        for (auto e : reg.view<ParticleEmitter>())
        {
            const auto& pe = reg.get<ParticleEmitter>(e);
            if (pe.layers.size() == 1 && pe.layers[0].name == "Layer 1") found = true;
        }
        IM_CHECK(found);
    }
    IM_CHECK(WriteTinyPrefab(scene, assets, "prefabs/__uitest_w0521b.prefab", "__uitest_w0521b_src"));
    sentinel = firstEntity();
    ExpectMcpKeepsSelection(ctx, ed, scene, sentinel, "prefabs/__uitest_w0521b.prefab", "prefab");
    std::filesystem::remove(assets + "prefabs/__uitest_w0521b.prefab", rmEc);

    // Undo で生成を戻したとき、選択が無効な id を指さない
    LogStep(ctx, "W05-21: Undo 後の選択");
    ed->ClearSelection();
    PendingSpawnRequest er; er.modelPath = "__empty__";
    ed->pendingSpawns.push_back(er);
    ctx->Yield(8);
    IM_CHECK_EQ(ed->selectedEntities.size(), (size_t)1);
    const entt::entity made = ed->selectedEntity;
    const size_t cntBefore = reg.storage<NameTag>().size();
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);
    ctx->Yield(8);
    IM_CHECK_EQ(reg.storage<NameTag>().size(), cntBefore - 1);   // Undo が本当に実行された
    IM_CHECK(!reg.valid(made));
    IM_CHECK(ed->selectedEntities.empty());
    IM_CHECK(ed->selectedEntity == entt::null);
}

// C13（BUG-45）: 新しく作る球（エディタ／MCP の生成経路）だけ分割数が上がり、既定の SpawnSphere
// （シーンの読み込み・スクリプトが通る経路）は従来の 16x16 のまま＝既存シーンの見た目は変わらない。
void T_SphereSubdivNewOnly(ImGuiTestContext* ctx)
{
    EditorContext* ed = Editor();
    Application*   app = App();
    if (!ed || !app || !app->GetScene()) { IM_ERRORF("Application / Scene を取得できません"); return; }
    Scene* scene = app->GetScene();
    auto& reg = scene->GetRegistry();

    auto indexCount = [&](entt::entity e) -> u32 {
        const auto* mr = reg.try_get<MeshRenderer>(e);
        return (mr && !mr->meshes.empty() && mr->meshes[0]) ? mr->meshes[0]->GetIndexCountLod(0) : 0u;
    };

    LogStep(ctx, "C13: 新しく作る球（生成経路）");
    ed->ClearSelection();
    PendingSpawnRequest sp; sp.modelPath = "__primitive_sphere__";
    ed->pendingSpawns.push_back(sp);
    ctx->Yield(8);
    IM_CHECK(ed->selectedEntity != entt::null && reg.valid(ed->selectedEntity));
    IM_CHECK_EQ(indexCount(ed->selectedEntity),
                Scene::kSphereSlicesNew * Scene::kSphereStacksNew * 6u);

    LogStep(ctx, "C13: 既定の SpawnSphere（読み込み経路）は従来のまま");
    Entity legacy = scene->SpawnSphere("__T_C13_Legacy", { 0.0f, -50.0f, 0.0f });
    IM_CHECK_EQ(indexCount(legacy.GetHandle()),
                Scene::kSphereSlicesLegacy * Scene::kSphereStacksLegacy * 6u);
    IM_CHECK_EQ(Scene::kSphereSlicesLegacy * Scene::kSphereStacksLegacy * 6u, 16u * 16u * 6u);

    // 複製（保存 / 読み込みと同じ経路）を通しても 64x32 のまま、旧い球は 16x16 のまま
    LogStep(ctx, "C13: 複製で分割数が保つ");
    const std::string assets = PathResolver::AssetsDir();
    const entt::entity src = ed->selectedEntity;
    const entt::entity dup = SceneSerializer::DuplicateEntity(*scene, src, assets);
    IM_CHECK(dup != entt::null);
    ctx->Yield(4);   // フレームの頭で ApplyPendingSphereSubdiv が走る
    IM_CHECK_EQ(indexCount(dup), Scene::kSphereSlicesNew * Scene::kSphereStacksNew * 6u);
    const entt::entity dupLegacy = SceneSerializer::DuplicateEntity(*scene, legacy.GetHandle(), assets);
    IM_CHECK(dupLegacy != entt::null);
    ctx->Yield(4);
    IM_CHECK_EQ(indexCount(dupLegacy), Scene::kSphereSlicesLegacy * Scene::kSphereStacksLegacy * 6u);

    // 後始末（後続のテストにエンティティと選択を残さない）
    for (entt::entity e : { dup, dupLegacy, legacy.GetHandle() })
        if (reg.valid(e)) reg.destroy(e);
    ctx->SetRef(kWinHierarchy);
    ctx->KeyPress(ImGuiKey_Z | ImGuiMod_Ctrl);   // 生成の Undo 履歴を 1 つ戻す（球本体も消える）
    ctx->Yield(8);
    IM_CHECK(!reg.valid(src));
    ed->ClearSelection();
}

} // namespace uitest
} // namespace dx12e
