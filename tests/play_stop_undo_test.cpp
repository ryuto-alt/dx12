// Play → Stop で Undo 履歴を保つ仕組みの単体テスト（W05-11）。GPU 不要。
//   ・SaveToString(writeEntityIds) の "_eid" → LoadFromString(preserveEntityIds) で全エンティティの id（版つき）が一致
//   ・Play 前の Undo 3 件は Stop 後も 1 件ずつ戻せる / Redo で戻る。Play 中に積んだ分は TruncateTo で捨てられる
//   ・"_eid" は通常の保存（SaveToString 既定 / SaveToStringV2）に出ない
//   ・id が使用中で作れないときは idsPreserved=false（呼び出し側が Undo を破棄する）
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る
#include "editor/UndoCore.h"
#include "editor/UndoSystem.h"   // 実コマンド（ComponentEditCommand / AddComponentCommand はヘッダ内で完結する）

#include <entt/entt.hpp>

#include <cstdio>
#include <string>
#include <vector>

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

entt::entity Add(Scene& s, const char* name, uint64_t guid)
{
    auto& reg = s.GetRegistry();
    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{ name });
    reg.emplace<Transform>(e);
    reg.emplace<EntityGuid>(e, EntityGuid{ guid });
    return e;
}

// entt の id で名前を書き換えるコマンド（実コマンドと同じく entity を直に握る）
class RenameCmd : public IUndoCommand
{
public:
    RenameCmd(Scene& s, entt::entity e, std::string from, std::string to)
        : m_s(s), m_e(e), m_from(std::move(from)), m_to(std::move(to)) {}
    void Undo() override { Set(m_from); }
    void Redo() override { Set(m_to); }
    const char* GetName() const override { return "Rename"; }
private:
    void Set(const std::string& n)
    {
        auto& reg = m_s.GetRegistry();
        if (reg.valid(m_e)) reg.get<NameTag>(m_e).name = n;
    }
    Scene& m_s; entt::entity m_e; std::string m_from, m_to;
};

std::string NameOf(Scene& s, entt::entity e)
{
    auto& reg = s.GetRegistry();
    return reg.valid(e) ? reg.get<NameTag>(e).name : std::string("<invalid>");
}
}

int main()
{
    Scene scene;
    auto& reg = scene.GetRegistry();

    // 版が 0 でない id・歯抜けの id を作る（作って消す → 再利用）
    const entt::entity tmp = Add(scene, "tmp", 0x99);
    reg.destroy(tmp);
    const entt::entity a = Add(scene, "A", 0xA1);   // tmp の index を再利用（版 1）
    const entt::entity gap = Add(scene, "gap", 0x98);
    const entt::entity b = Add(scene, "B", 0xB2);
    const entt::entity c = Add(scene, "C", 0xC3);
    reg.destroy(gap);

    // 編集 3 回
    UndoSystem undo;
    reg.get<NameTag>(a).name = "A1"; undo.PushCommand(std::make_unique<RenameCmd>(scene, a, "A", "A1"));
    reg.get<NameTag>(b).name = "B1"; undo.PushCommand(std::make_unique<RenameCmd>(scene, b, "B", "B1"));
    reg.get<NameTag>(c).name = "C1"; undo.PushCommand(std::make_unique<RenameCmd>(scene, c, "C", "C1"));
    CHECK(undo.UndoDepth() == 3);

    // ── Play 開始 ──
    const std::string snap = SceneSerializer::SaveToString(scene, "", /*writeEntityIds=*/true);
    CHECK(snap.find("\"_eid\"") != std::string::npos);
    const uint64_t playSeq = undo.PushSeq(), humanSeq = undo.LastHumanEditSeq();
    // 通常の保存には "_eid" を出さない
    CHECK(SceneSerializer::SaveToString(scene, "").find("_eid") == std::string::npos);
    CHECK(SceneSerializer::SaveToStringV2(scene, "").find("_eid") == std::string::npos);

    // Play 中: 作る・消す・Undo を積む
    const entt::entity d = Add(scene, "D", 0xD4);
    reg.destroy(b);
    undo.PushCommand(std::make_unique<RenameCmd>(scene, d, "D", "D1"));
    CHECK(undo.UndoDepth() == 4);

    // ── Stop: 復元 ──
    scene.Clear();
    SceneSerializer::SceneLoadOptions opts;
    opts.preserveEntityIds = true;
    CHECK(SceneSerializer::LoadFromString(scene, snap, "", &opts));
    CHECK(opts.idsPreserved);
    undo.TruncateTo(playSeq, humanSeq);

    CHECK(reg.valid(a) && reg.valid(b) && reg.valid(c));          // 版つきで全一致
    CHECK(NameOf(scene, a) == "A1" && NameOf(scene, b) == "B1" && NameOf(scene, c) == "C1");
    CHECK(!reg.valid(d));
    CHECK(FindEntityByGuid(reg, 0xA1) == a && FindEntityByGuid(reg, 0xB2) == b && FindEntityByGuid(reg, 0xC3) == c);
    CHECK(undo.UndoDepth() == 3);

    // Undo を 1 回ずつ
    undo.Undo(); CHECK(NameOf(scene, c) == "C" && NameOf(scene, b) == "B1");
    undo.Undo(); CHECK(NameOf(scene, b) == "B" && NameOf(scene, a) == "A1");
    undo.Undo(); CHECK(NameOf(scene, a) == "A");
    CHECK(!undo.CanUndo());
    // Redo で戻る
    undo.Redo(); undo.Redo(); undo.Redo();
    CHECK(NameOf(scene, a) == "A1" && NameOf(scene, b) == "B1" && NameOf(scene, c) == "C1");

    // 復元後に保存し直しても "_eid" は出ない（読み込み側は一時フィールドを保持しない）
    CHECK(SceneSerializer::SaveToString(scene, "").find("_eid") == std::string::npos);

    // ── id が使用中で作れないとき: idsPreserved=false ──
    {
        Scene s2;
        auto& r2 = s2.GetRegistry();
        // snap の a と同じ index を別のエンティティで塞ぐ（Clear は snap 読み込み側が行うので、
        // Load の前に Spawn 相当の create が割り込む状況を、不整合な "_eid" の重複で作る）
        std::string dup = snap;
        const std::string key = "\"_eid\":" + std::to_string(static_cast<uint64_t>(entt::to_integral(b)));
        const size_t pos = dup.find(key);
        CHECK(pos != std::string::npos);
        if (pos != std::string::npos)
            dup.replace(pos, key.size(), "\"_eid\":" + std::to_string(static_cast<uint64_t>(entt::to_integral(a))));
        SceneSerializer::SceneLoadOptions o2;
        o2.preserveEntityIds = true;
        CHECK(SceneSerializer::LoadFromString(s2, dup, "", &o2));
        CHECK(!o2.idsPreserved);
        (void)r2;
    }

    // ── 実コマンド: ComponentEditCommand<Transform> / AddComponentCommand<RigidBody> をまたぐ ──
    {
        Scene s3;
        auto& r3 = s3.GetRegistry();
        const entt::entity x = Add(s3, "X", 0x31);
        UndoSystem u3;
        Transform before = r3.get<Transform>(x), after = before;
        after.position.x = 5.0f;
        r3.get<Transform>(x) = after;
        u3.PushCommand(std::make_unique<ComponentEditCommand<Transform>>(&r3, x, before, after, "Move"));
        RigidBody rb;                       // bodyId は既定（未登録）。ランタイムの bodyId は Play 中の物理が入れる
        r3.emplace<RigidBody>(x, rb);
        u3.PushCommand(std::make_unique<AddComponentCommand<RigidBody>>(&r3, x, rb, "AddRigidBody"));
        CHECK(u3.UndoDepth() == 2);
        const std::string snap3 = SceneSerializer::SaveToString(s3, "", true);
        const uint64_t ps = u3.PushSeq(), hs = u3.LastHumanEditSeq();
        // Play 中に別の編集を積む
        r3.get<Transform>(x).position.x = 9.0f;
        u3.PushCommand(std::make_unique<ComponentEditCommand<Transform>>(&r3, x, after, r3.get<Transform>(x), "Move2"));
        s3.Clear();
        SceneSerializer::SceneLoadOptions o3; o3.preserveEntityIds = true;
        CHECK(SceneSerializer::LoadFromString(s3, snap3, "", &o3));
        CHECK(o3.idsPreserved);
        u3.TruncateTo(ps, hs);
        CHECK(u3.UndoDepth() == 2 && r3.valid(x));
        u3.Undo();                           // RigidBody を外す
        CHECK(!r3.all_of<RigidBody>(x));
        u3.Redo();                           // 付け直す（bodyId は捕捉時のまま = 未登録）
        CHECK(r3.all_of<RigidBody>(x) && r3.get<RigidBody>(x).bodyId == kInvalidBodyId);
        u3.Undo(); u3.Undo();                // 外す → Transform を戻す
        CHECK(r3.get<Transform>(x).position.x == before.position.x);
        u3.Redo(); u3.Redo();
        CHECK(r3.get<Transform>(x).position.x == 5.0f);
    }

    std::printf("play_stop_undo_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
