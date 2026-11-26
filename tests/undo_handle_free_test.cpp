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

    std::puts("section 4");
    // ── Delete を Play 前に積み、Stop をまたいで Undo / Redo（外部親の id が再読込で保たれること）──
    {
        Scene scene;
        auto& reg = scene.GetRegistry();
        const entt::entity parent = Add(scene, "Parent", 0xE1);
        const entt::entity kid = Add(scene, "Kid", 0xE2);
        const entt::entity kidChild = Add(scene, "KidChild", 0xE3);
        reg.get<Transform>(kid).parent = parent;
        reg.get<Transform>(kidChild).parent = kid;
        UndoSystem undo;

        // kid のサブツリー（kid + kidChild）を削除
        std::vector<DeletedEntityRecord> recs;
        recs.push_back({ SceneSerializer::SerializeEntity(scene, kid, ""), -1 });
        recs.push_back({ SceneSerializer::SerializeEntity(scene, kidChild, ""), 0 });
        reg.destroy(kidChild);
        reg.destroy(kid);
        undo.PushCommand(std::make_unique<DeleteEntityCommand>(&scene, "", std::move(recs),
            std::vector<entt::entity>{ kid, kidChild }, parent));
        CHECK(reg.view<NameTag>().size() == 1);

        // Play → Stop
        const uint64_t ps = undo.PushSeq(), hs = undo.LastHumanEditSeq();
        const std::string snap = SceneSerializer::SaveToString(scene, "", true);
        CHECK(StopRestore(scene, snap));
        undo.TruncateTo(ps, hs);
        CHECK(undo.UndoDepth() == 1);
        CHECK(reg.valid(parent));

        undo.Undo();
        CHECK(reg.view<NameTag>().size() == 3);
        const entt::entity k2 = FindEntityByGuid(reg, 0xE2);
        const entt::entity c2 = FindEntityByGuid(reg, 0xE3);
        CHECK(k2 != entt::null && c2 != entt::null);
        CHECK(reg.get<Transform>(k2).parent == parent);          // 外部親が戻る
        CHECK(reg.get<Transform>(c2).parent == k2);              // 内側の親子も戻る
        undo.Redo();
        CHECK(reg.view<NameTag>().size() == 1);
        undo.Undo();
        CHECK(reg.view<NameTag>().size() == 3);
        undo.Redo();
        CHECK(!undo.CanRedo());
    }

    std::puts("section 5");
    // ── Prefab 生成 / Revert（親 + 子のサブツリー）を Stop の前後でまたぐ。SpawnPrefabCommand / PrefabRevertCommand は
    //    エンティティと JSON しか持たないので、実プレハブ資産が無くてもサブツリーで確かめられる。
    //    （2 つを同じ履歴に積むと、Revert がサブツリーを作り直して id が変わり、生成側の古い id が効かない。
    //      これは W05-11 以前からのコマンド設計の弱点なので、別々の履歴で確かめる）──
    for (int variant = 0; variant < 2; ++variant)
    {
        Scene scene;
        auto& reg = scene.GetRegistry();
        const entt::entity holder = Add(scene, "Holder", 0xF0);
        const entt::entity root = Add(scene, "PrefabRoot", 0xF1);
        const entt::entity child = Add(scene, "PrefabChild", 0xF2);
        reg.get<Transform>(root).parent = holder;
        reg.get<Transform>(child).parent = root;
        reg.get<Transform>(child).position = { 1.0f, 2.0f, 3.0f };
        UndoSystem undo;
        if (variant == 0)
        {
            undo.PushCommand(std::make_unique<SpawnPrefabCommand>(&scene, "", std::vector<entt::entity>{ root, child }));
        }
        else
        {
            // Revert: before = 変更前のサブツリー JSON、after = 今のサブツリー（子の位置が違う）
            const std::string beforeJson = SceneSerializer::SerializeSubtree(scene, root, "");
            reg.get<Transform>(child).position = { 9.0f, 9.0f, 9.0f };
            undo.PushCommand(std::make_unique<PrefabRevertCommand>(&scene, "", beforeJson,
                std::vector<entt::entity>{ root, child }, holder));
        }

        const uint64_t ps = undo.PushSeq(), hs = undo.LastHumanEditSeq();
        const std::string snap = SceneSerializer::SaveToString(scene, "", true);
        CHECK(StopRestore(scene, snap));
        undo.TruncateTo(ps, hs);
        CHECK(undo.UndoDepth() == 1);

        if (variant == 0)
        {
            undo.Undo();   // 生成を戻す
            CHECK(reg.view<NameTag>().size() == 1);
            undo.Redo();   // JSON から再生成（外部親 holder を張り直す）
            CHECK(reg.view<NameTag>().size() == 3);
            const entt::entity r = FindEntityByGuid(reg, 0xF1);
            const entt::entity c = FindEntityByGuid(reg, 0xF2);
            CHECK(r != entt::null && c != entt::null);
            CHECK(reg.get<Transform>(r).parent == holder && reg.get<Transform>(c).parent == r);
            undo.Undo(); undo.Redo();
            CHECK(reg.view<NameTag>().size() == 3);
        }
        else
        {
            undo.Undo();   // Revert を戻す → before のサブツリー（位置 1,2,3）
            {
                const entt::entity c = FindEntityByGuid(reg, 0xF2);
                const entt::entity r = FindEntityByGuid(reg, 0xF1);
                CHECK(c != entt::null && r != entt::null && reg.get<Transform>(c).position.x == 1.0f);
                CHECK(reg.get<Transform>(r).parent == holder && reg.get<Transform>(c).parent == r);
            }
            undo.Redo();   // Revert（位置 9,9,9）
            {
                const entt::entity c = FindEntityByGuid(reg, 0xF2);
                CHECK(c != entt::null && reg.get<Transform>(c).position.x == 9.0f);
            }
            undo.Undo();
            CHECK(reg.view<NameTag>().size() == 3);
        }
    }

    std::puts("section 6");
    // ── 実行時状態を持つ他の型の特殊化 ──
    {
        AudioSource a; a.volume = 0.5f; a.runtimeSlot = 7; a.startedThisPlay = true;
        const AudioSource ac = UndoValueTraits<AudioSource>::Capture(a);
        CHECK(ac.runtimeSlot == -1 && !ac.startedThisPlay && ac.volume == 0.5f);
        AudioSource live; live.runtimeSlot = 3; live.startedThisPlay = true; live.volume = 1.0f;
        UndoValueTraits<AudioSource>::Merge(live, ac);
        CHECK(live.volume == 0.5f && live.runtimeSlot == 3 && live.startedThisPlay);

        CharacterController cc; cc.mass = 5.0f; cc._registered = true; cc._grounded = true;
        const CharacterController ccc = UndoValueTraits<CharacterController>::Capture(cc);
        CHECK(!ccc._registered && !ccc._grounded && ccc.mass == 5.0f);
        CharacterController lc; lc._registered = true; lc._grounded = true; lc.mass = 1.0f;
        UndoValueTraits<CharacterController>::Merge(lc, ccc);
        CHECK(lc.mass == 5.0f && lc._registered && lc._grounded);

        NetworkIdentity ni; ni.interestRadius = 4.0f; ni._netId = 99; ni._owner = 2; ni._isLocalOwner = true;
        const NetworkIdentity nc = UndoValueTraits<NetworkIdentity>::Capture(ni);
        CHECK(nc._netId == 0 && nc._owner == 0 && !nc._isLocalOwner && nc.interestRadius == 4.0f);
        NetworkIdentity ln; ln._netId = 5; ln.interestRadius = 1.0f;
        UndoValueTraits<NetworkIdentity>::Merge(ln, nc);
        CHECK(ln.interestRadius == 4.0f && ln._netId == 5);

        FoliageLayer fl; fl.cullDistance = 123.0f;
        int dummy = 0;
        fl._diskSet = &dummy;
        const FoliageLayer fc = UndoValueTraits<FoliageLayer>::Capture(fl);
        CHECK(fc._diskSet == nullptr && fc.cullDistance == 123.0f);
        FoliageLayer lf; int other = 0; lf._diskSet = &other; lf.cullDistance = 1.0f;
        UndoValueTraits<FoliageLayer>::Merge(lf, fc);
        CHECK(lf.cullDistance == 123.0f && lf._diskSet == &other);

        // LuaScript: started / loadError / errorMessage も生きている側のまま（env と一組）
        LuaScript ls; ls.scriptPath = "x.lua"; ls.started = true; ls.loadError = true; ls.errorMessage = "boom";
        const LuaScript lc2 = UndoValueTraits<LuaScript>::Capture(ls);
        CHECK(!lc2.started && !lc2.loadError && lc2.errorMessage.empty());
        LuaScript live2; live2.started = true; live2.loadError = false; live2.errorMessage = "live";
        UndoValueTraits<LuaScript>::Merge(live2, lc2);
        CHECK(live2.scriptPath == "x.lua" && live2.started && !live2.loadError && live2.errorMessage == "live");

        // MeshRenderer は純データから付け直せない（Add / Remove は static_assert で禁止）
        static_assert(!UndoValueTraits<MeshRenderer>::kSupportsEmplace);
        // MeshRenderer の Merge は vgAutoPath を生きている側のまま、_remapKey は再解決へ
        MeshRenderer mlive; mlive.vgAutoPath = "live.vgeo"; mlive._remapKey = 5;
        MeshRenderer mst; mst.vgAutoPath = "stored.vgeo";
        UndoValueTraits<MeshRenderer>::Merge(mlive, UndoValueTraits<MeshRenderer>::Capture(mst));
        CHECK(mlive.vgAutoPath == "live.vgeo" && mlive._remapKey == 0);
    }

    std::printf("undo_handle_free_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
