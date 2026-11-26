// コンポーネントのシリアライザ登録表（W05-15）と、ポストの V2 マクロの単体テスト。GPU 不要。
//   ・.inc の登録の整合（typeName の重複なし / 必須の関数あり）と、登録の順序
//   ・未知の型の往復（エンティティ JSON → 別シーン）。skipOnSave の型は書かれない。"_eid" は通常の保存に出ない
//   ・登録が空のとき、保存結果にレジストリ由来のキーが出ない
//   ・V2 のダミーのフィールドは、既定のままなら JSON にもハッシュにも出ず、違えば出る（往復する）
#include "core/SequencerApply.h"
#include "scene/ComponentSerializerRegistry.h"
#include "scene/PostV2Fields.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace dx12e;
using namespace dx12e::seqhost;
using nlohmann::json;

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

// ---- ダミーのコンポーネント（Components.h に無い型） ----
struct DummyComp { int a = 0; std::string s; };
struct DummyTransient { int x = 0; };

bool DummyHas(const entt::registry& r, entt::entity e) { return r.all_of<DummyComp>(e); }
void DummySave(const entt::registry& r, entt::entity e, json& out)
{
    const auto& d = r.get<DummyComp>(e);
    out = { {"a", d.a}, {"s", d.s} };
}
void DummyLoad(entt::registry& r, entt::entity e, const json& in)
{
    DummyComp d;
    d.a = in.value("a", 0);
    d.s = in.value("s", std::string());
    r.emplace_or_replace<DummyComp>(e, std::move(d));
}
void DummyRemove(entt::registry& r, entt::entity e) { r.remove<DummyComp>(e); }

bool TransHas(const entt::registry& r, entt::entity e) { return r.all_of<DummyTransient>(e); }
void TransSave(const entt::registry&, entt::entity, json& out) { out = { {"x", 1} }; }
void TransLoad(entt::registry& r, entt::entity e, const json&) { r.emplace_or_replace<DummyTransient>(e); }
bool TransSkip(const entt::registry&, entt::entity) { return true; }

entt::entity Add(Scene& s, const char* name, uint64_t guid)
{
    auto& reg = s.GetRegistry();
    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{ name });
    reg.emplace<Transform>(e);
    reg.emplace<EntityGuid>(e, EntityGuid{ guid });
    return e;
}

// ---- V2 マクロのダミー（PostProcessSettings とは無関係に、同じ部品で確かめる） ----
struct FakePost { bool on = false; float amount = 0.5f; int mode = 2; DirectX::XMFLOAT3 tint{1, 1, 1}; std::string path; };
#define FAKE_V2(B, F, I, V, S) B(on) F(amount) I(mode) V(tint) S(path)

json SaveFake(const FakePost& p)
{
    static const FakePost def{};
    json j = json::object();
    UNO_POST_V2_SAVE(FAKE_V2, p, def, j)
    return j;
}
FakePost LoadFake(const json& j)
{
    FakePost p;
    UNO_POST_V2_LOAD(FAKE_V2, p, j)
    return p;
}
constexpr uint64_t kFnvInit = 1469598103934665603ull;
uint64_t HashFake(const FakePost& p)
{
    static const FakePost def{};
    uint64_t v = kFnvInit;
    auto mix = [&v](const void* q, size_t n) {
        const auto* b = static_cast<const unsigned char*>(q);
        for (size_t i = 0; i < n; ++i) { v ^= b[i]; v *= 1099511628211ull; }
    };
    UNO_POST_V2_HASH(FAKE_V2, p, def, mix)
    return v;
}
} // namespace

int main()
{
    // ── 登録の整合 ──
    {
        const auto& list = ComponentSerializers();
        std::set<std::string> names;
        bool ok = true;
        for (const auto& cs : list)
            ok = ok && cs.typeName && cs.has && cs.save && cs.load && cs.remove && names.insert(cs.typeName).second;
        CHECK(ok);
        // .inc を展開し直しても同じ並び
        std::vector<ComponentSerializer> again;
        RegisterAllComponentSerializers(again);
        CHECK(again.size() == list.size());
        for (size_t i = 0; i < again.size() && i < list.size(); ++i)
            CHECK(std::strcmp(again[i].typeName, list[i].typeName) == 0);
    }

    const size_t baseCount = ComponentSerializers().size();
    {
        // 登録が .inc だけのとき、レジストリ由来のキーは出ない
        Scene scene;
        const entt::entity e = Add(scene, "E", 0x11);
        const json ej = json::parse(SceneSerializer::SerializeEntity(scene, e, ""));
        CHECK(!ej.contains("dummyTest"));
    }

    // ── 順序と検索（テスト用の登録） ──
    ComponentSerializer d1; d1.typeName = "dummyTest"; d1.has = DummyHas; d1.save = DummySave; d1.load = DummyLoad; d1.remove = DummyRemove;
    ComponentSerializer d2; d2.typeName = "transientTest"; d2.has = TransHas; d2.save = TransSave; d2.load = TransLoad; d2.skipOnSave = TransSkip;
    TestOnlyAppendComponentSerializer(d1);
    TestOnlyAppendComponentSerializer(d2);
    CHECK(ComponentSerializers().size() == baseCount + 2);
    CHECK(std::strcmp(ComponentSerializers()[baseCount].typeName, "dummyTest") == 0);
    CHECK(std::strcmp(ComponentSerializers()[baseCount + 1].typeName, "transientTest") == 0);
    CHECK(FindComponentSerializer("dummyTest") == &ComponentSerializers()[baseCount]);
    CHECK(FindComponentSerializer("nothing") == nullptr);

    // ── 未知の型の往復 ──
    {
        Scene src;
        const entt::entity e = Add(src, "Thing", 0x22);
        src.GetRegistry().emplace<DummyComp>(e, DummyComp{ 42, "hello" });
        src.GetRegistry().emplace<DummyTransient>(e);

        const json ej = json::parse(SceneSerializer::SerializeEntity(src, e, ""));
        CHECK(ej.contains("dummyTest"));
        CHECK(ej.value("dummyTest", json::object()).value("a", 0) == 42);
        CHECK(!ej.contains("transientTest"));   // skipOnSave

        Scene dst;
        const entt::entity e2 = SceneSerializer::InstantiateEntity(dst, ej.dump(), "");
        CHECK(e2 != entt::null);
        if (e2 != entt::null)
        {
            const auto* d = dst.GetRegistry().try_get<DummyComp>(e2);
            CHECK(d && d->a == 42 && d->s == "hello");
            CHECK(!dst.GetRegistry().all_of<DummyTransient>(e2));
        }

        // シーン全体の保存: 型は往復し、_eid は通常の保存には（レジストリ経由の型があっても）出ない
        const std::string plain = SceneSerializer::SaveToString(src, "");
        CHECK(plain.find("dummyTest") != std::string::npos);
        CHECK(plain.find("_eid") == std::string::npos);
        CHECK(SceneSerializer::SaveToStringV2(src, "").find("_eid") == std::string::npos);

        // Play 用スナップショットは _eid つき。レジストリ経由の型もその id で復元される
        const std::string snap = SceneSerializer::SaveToString(src, "", /*writeEntityIds=*/true);
        CHECK(snap.find("_eid") != std::string::npos);
        Scene back;
        SceneSerializer::SceneLoadOptions opts;
        opts.preserveEntityIds = true;
        CHECK(SceneSerializer::LoadFromString(back, snap, "", &opts));
        CHECK(opts.idsPreserved);
        CHECK(back.GetRegistry().valid(e) && back.GetRegistry().all_of<DummyComp>(e));
    }

    TestOnlyResetComponentSerializers();
    CHECK(ComponentSerializers().size() == baseCount);

    // ── V2 マクロ（ダミーのフィールド） ──
    {
        FakePost def;
        CHECK(SaveFake(def).empty());               // 既定なら JSON に何も出ない
        CHECK(HashFake(def) == kFnvInit);           // ハッシュは初期値のまま（混ざらない）

        FakePost p = def;
        p.amount = 0.9f; p.tint = { 0.1f, 0.2f, 0.3f }; p.path = "x.png"; p.on = true; p.mode = 5;
        const json j = SaveFake(p);
        CHECK(j.size() == 5);
        CHECK(HashFake(p) != HashFake(def));
        const FakePost q = LoadFake(j);
        CHECK(q.on && q.mode == 5 && q.path == "x.png" && q.amount == 0.9f && q.tint.y == 0.2f);

        FakePost one = def; one.amount = 0.6f;
        const json j1 = SaveFake(one);
        CHECK(j1.size() == 1 && j1.contains("amount"));   // 違うものだけ
        CHECK(LoadFake(json::object()).mode == 2);        // キーが無ければ既定
    }

    // ── ポスト表: V2 の値が V1 末尾の XxxOn を継承しない ──
    {
        std::vector<PostFieldInfo> t(5);
        t[0].name = "aOn";    t[0].kind = 'B';   // V1
        t[1].name = "aAmt";   t[1].kind = 'F';
        t[2].name = "debandOn"; t[2].kind = 'B'; // V1 の末尾
        t[3].name = "v2Val";  t[3].kind = 'F';   // V2: 自分の On が無い
        t[4].name = "v2Flag"; t[4].kind = 'B';
        AssignPostOnIndices(t, 0, 3);
        AssignPostOnIndices(t, 3, t.size());
        CHECK(t[1].onIndex == 0);
        CHECK(t[3].onIndex == -1);   // 継承しない（debandOn = 2 になってはいけない）
        // 実際の表でも、V1 の末尾 XxxOn を指す V2 の値が無い（今は V2 が空なので一致するだけ）
        const auto& real = PostFieldTable();
        CHECK(!real.empty());
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
