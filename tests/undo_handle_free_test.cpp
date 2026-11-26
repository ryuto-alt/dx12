// Undo コマンドは実行時ハンドルを持たない（W05-11 恒久策 / EXEC §0.3b）の回帰テスト。GPU 不要。
//   ・ComponentEditCommand<MeshRenderer>: Mesh* / Material* を保持しない。Stop 相当（持ち主の差し替え）の後でも、
//     Undo / Redo は純データだけを戻し、生きている側のハンドルを残す
//   ・RigidBody の bodyId / LuaScript の env・self も同様
//   ・Spawn / Delete / Group を Stop（Scene::Clear + id 保存の再読込）の前後でまたいで Undo / Redo が往復する
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"
#include "editor/UndoCore.h"
#include "editor/UndoSystem.h"

#include <entt/entt.hpp>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

using namespace dx12e;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// 参照しない目印のポインタ（解放済み / 別物を表す）。デリファレンスされればクラッシュするので、触られれば分かる。
Mesh* FakeMesh(uintptr_t v) { return reinterpret_cast<Mesh*>(v); }
Material* FakeMat(uintptr_t v) { return reinterpret_cast<Material*>(v); }

entt::entity Add(Scene& s, const char* name, uint64_t guid)
{
    auto& reg = s.GetRegistry();
    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{ name });
    reg.emplace<Transform>(e);
    reg.emplace<EntityGuid>(e, EntityGuid{ guid });
    return e;
}

// Stop 相当: Scene を作り直して id を保つ。
bool StopRestore(Scene& scene, const std::string& snap)
{
    scene.Clear();
    SceneSerializer::SceneLoadOptions opts;
    opts.preserveEntityIds = true;
    return SceneSerializer::LoadFromString(scene, snap, "", &opts) && opts.idsPreserved;
}
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // ── Traits 単体: Capture はハンドルを空にし、Merge は生きている側のハンドルを残す ──
    {
        MeshRenderer mr;
        mr.modelPath = "a.glb";
        mr.meshes = { FakeMesh(0x1000), FakeMesh(0x2000) };
        mr.materials = { FakeMat(0x3000) };
        mr.meshNodeTransforms.resize(2);
        mr.overrideMetallic = 0.5f;
        const MeshRenderer c = UndoValueTraits<MeshRenderer>::Capture(mr);
        CHECK(c.meshes.empty() && c.materials.empty() && c.meshNodeTransforms.empty());
        CHECK(c.overrideMetallic == 0.5f && c.modelPath == "a.glb");

        MeshRenderer live;
        live.modelPath = "a.glb";
        live.meshes = { FakeMesh(0x9000) };
        live.materials = { FakeMat(0xA000) };
        live.overrideMetallic = 0.9f;
        UndoValueTraits<MeshRenderer>::Merge(live, c);
        CHECK(live.overrideMetallic == 0.5f);
        CHECK(live.meshes.size() == 1 && live.meshes[0] == FakeMesh(0x9000));
        CHECK(live.materials.size() == 1 && live.materials[0] == FakeMat(0xA000));
    }

    std::puts("section 1");
    // ── ComponentEditCommand<MeshRenderer>: Stop をまたいで Undo / Redo ──
    {
        Scene scene;
        auto& reg = scene.GetRegistry();
        const entt::entity e = Add(scene, "Box", 0x11);
        MeshRenderer mr;
        mr.modelPath = "box.glb";
        mr.meshes = { FakeMesh(0x1000) };          // Play 前の Mesh（Stop で解放される扱い）
        reg.emplace<MeshRenderer>(e, mr);

        UndoSystem undo;
        const MeshRenderer before = reg.get<MeshRenderer>(e);
        reg.get<MeshRenderer>(e).overrideMetallic = 0.75f;
        undo.PushCommand(std::make_unique<ComponentEditCommand<MeshRenderer>>(&reg, e, before, reg.get<MeshRenderer>(e), "Material"));
        // Stop 相当: Scene::Clear → 再読込で Mesh が作り直される。ここでは持ち主の差し替えだけを真似る
        // （シリアライザは Mesh* を実際に読むので、目印のポインタのまま保存・復元はしない。保存・復元の往復は下の節で行う）。
        reg.get<MeshRenderer>(e).meshes.clear();
        reg.get<MeshRenderer>(e).materials.clear();
        reg.get<MeshRenderer>(e).meshes = { FakeMesh(0x7000) };
        reg.get<MeshRenderer>(e).materials = { FakeMat(0x7100) };
        CHECK(undo.UndoDepth() == 1);

        undo.Undo();
        CHECK(reg.get<MeshRenderer>(e).overrideMetallic == before.overrideMetallic);
        CHECK(reg.get<MeshRenderer>(e).meshes.size() == 1 && reg.get<MeshRenderer>(e).meshes[0] == FakeMesh(0x7000));
        CHECK(reg.get<MeshRenderer>(e).materials.size() == 1 && reg.get<MeshRenderer>(e).materials[0] == FakeMat(0x7100));
        undo.Redo();
        CHECK(reg.get<MeshRenderer>(e).overrideMetallic == 0.75f);
        CHECK(reg.get<MeshRenderer>(e).meshes[0] == FakeMesh(0x7000));
    }

    std::puts("section 2");
    // ── RigidBody の bodyId / LuaScript の env・self は書き戻さない ──
    {
        Scene scene;
        auto& reg = scene.GetRegistry();
        const entt::entity e = Add(scene, "X", 0x21);
        RigidBody rb;
        rb.bodyId = 42;                          // Play 中のボディ（捕捉時）
        rb.mass = 1.0f;
        reg.emplace<RigidBody>(e, rb);
        RigidBody after = rb; after.mass = 3.0f;
        UndoSystem undo;
        undo.PushCommand(std::make_unique<ComponentEditCommand<RigidBody>>(&reg, e, rb, after, "RB"));
        reg.get<RigidBody>(e) = after;
        reg.get<RigidBody>(e).bodyId = 77;       // いまの（別の）ボディ
        undo.Undo();
        CHECK(reg.get<RigidBody>(e).mass == 1.0f && reg.get<RigidBody>(e).bodyId == 77);
        undo.Redo();
        CHECK(reg.get<RigidBody>(e).mass == 3.0f && reg.get<RigidBody>(e).bodyId == 77);

        // Add は bodyId を持ち越さず、付け直しても未登録のまま
        const entt::entity f = Add(scene, "Y", 0x22);
        reg.emplace<RigidBody>(f, rb);
        undo.PushCommand(std::make_unique<AddComponentCommand<RigidBody>>(&reg, f, rb, "AddRB"));
        undo.Undo();
        CHECK(!reg.all_of<RigidBody>(f));
        undo.Redo();
        CHECK(reg.all_of<RigidBody>(f) && reg.get<RigidBody>(f).bodyId == kInvalidBodyId);

        // LuaScript: shared_ptr を握らない（use_count が増えない）
        auto env = std::make_shared<int>(1);
        LuaScript ls;
        ls.scriptPath = "a.lua";
        ls.env = env;
        reg.emplace<LuaScript>(e, ls);
        LuaScript ls2 = ls; ls2.enabled = !ls.enabled;
        const long baseCount = env.use_count();
        {
            auto cmd = std::make_unique<ComponentEditCommand<LuaScript>>(&reg, e, ls, ls2, "Lua");
            CHECK(env.use_count() == baseCount);        // コマンドは env を持たない
            cmd->Undo();
            CHECK(reg.get<LuaScript>(e).env == env);    // 生きている側の env は残る
            CHECK(reg.get<LuaScript>(e).enabled == ls.enabled);
        }
    }

    std::puts("section 3");
    // ── Spawn / Delete / Group を Stop の前後でまたぐ ──
    {
        Scene scene;
        auto& reg = scene.GetRegistry();
        const entt::entity a = Add(scene, "A", 0xA1);
        const entt::entity b = Add(scene, "B", 0xB2);
        UndoSystem undo;

        // Group: A, B を新しい親へ
        const entt::entity grp = reg.create();
        reg.emplace<NameTag>(grp, NameTag{ "Group" });
        reg.emplace<Transform>(grp);
        reg.emplace<EntityGuid>(grp, EntityGuid{ 0xC3 });
        reg.get<Transform>(a).parent = grp;
        reg.get<Transform>(b).parent = grp;
        undo.PushCommand(std::make_unique<GroupCommand>(&reg, "Group", grp, entt::null,
            std::vector<std::pair<entt::entity, entt::entity>>{ { a, entt::null }, { b, entt::null } }));

        // Spawn（Undo で消えて Redo で戻る）
        const entt::entity sp = Add(scene, "Spawned", 0xD4);
        undo.PushCommand(std::make_unique<SpawnEntityCommand>(&scene, "", sp));

        const uint64_t ps = undo.PushSeq(), hs = undo.LastHumanEditSeq();
        const std::string snap = SceneSerializer::SaveToString(scene, "", true);
        CHECK(StopRestore(scene, snap));
        undo.TruncateTo(ps, hs);
        CHECK(undo.UndoDepth() == 2);
        CHECK(reg.valid(a) && reg.valid(b) && reg.valid(grp) && reg.valid(sp));

        // Stop の後で Undo / Redo を往復
        undo.Undo();   // Spawn を消す
        CHECK(reg.view<NameTag>().size() == 3);
        undo.Undo();   // Group を解く
        CHECK(!reg.valid(grp) && reg.get<Transform>(a).parent == entt::null && reg.get<Transform>(b).parent == entt::null);
        undo.Redo();   // Group
        CHECK(reg.get<Transform>(a).parent != entt::null && reg.valid(reg.get<Transform>(a).parent));
        undo.Redo();   // Spawn
        CHECK(reg.view<NameTag>().size() == 4);
        undo.Undo(); undo.Undo();
        CHECK(reg.view<NameTag>().size() == 2);
        undo.Redo(); undo.Redo();
        CHECK(reg.view<NameTag>().size() == 4);

        // Delete（Stop 後に積む）
        const entt::entity victim = FindEntityByGuid(reg, 0xA1);
        CHECK(victim != entt::null);
        std::vector<DeletedEntityRecord> recs;
        recs.push_back({ SceneSerializer::SerializeEntity(scene, victim, ""), -1 });
        const entt::entity oldParent = reg.get<Transform>(victim).parent;
        reg.destroy(victim);
        undo.PushCommand(std::make_unique<DeleteEntityCommand>(&scene, "", std::move(recs),
            std::vector<entt::entity>{ victim }, oldParent));
        CHECK(reg.view<NameTag>().size() == 3);
        undo.Undo();
        CHECK(reg.view<NameTag>().size() == 4);
        undo.Redo();
        CHECK(reg.view<NameTag>().size() == 3);
        undo.Undo();
        undo.Undo(); undo.Undo(); undo.Undo();   // Delete → Spawn → Group まで全部戻る
        CHECK(!undo.CanUndo());
    }

    std::printf("undo_handle_free_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
