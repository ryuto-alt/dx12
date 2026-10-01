// インスタンス群（InstanceGroup。docs/SCENE_FORMAT_DESIGN.md §4.1 の 4-1）の GPU 不要テスト。
//
// 押さえるもの（段階 A）:
//   サイドカーの 1 行: 3 / 6 / 9 個の省略形・最短 float・ビット往復（-0.0 や極小値を含む）・壊れた行の扱い
//   InstanceSet: 行列が Transform::GetWorldMatrix と同じ・コピーオンライト
//   シーンの保存: <シーン>.inst/<guid>.jsonl が書かれ、再保存でバイト一致、内容が同じファイルには触らない、孤児は消える
//   読み込み: 往復で実体が一致 / スナップショット（SaveToString → LoadFromString）は実体を共有する（再直列化しない）
//   複製（DuplicateEntity）は実体を共有する

#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/InstanceGroupIO.h"
#include "scene/Entity.h"
#include "ecs/Components.h"
#include "ecs/InstanceGroup.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る

#include <entt/entt.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::instgroup;
namespace fs = std::filesystem;

namespace
{
int g_failures = 0;
int g_checks   = 0;
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static std::string ReadAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void WriteAll(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

static u32 Bits(f32 f) { u32 u; std::memcpy(&u, &f, 4); return u; }
static bool SameBits(const InstanceTRS& a, const InstanceTRS& b) { return BitEqual(a, b); }

static InstanceTRS Make(f32 px, f32 py, f32 pz, f32 rx = 0, f32 ry = 0, f32 rz = 0, f32 sx = 1, f32 sy = 1, f32 sz = 1)
{
    InstanceTRS t;
    t.p = {px, py, pz}; t.r = {rx, ry, rz}; t.s = {sx, sy, sz};
    return t;
}

static void Test_LineForms()
{
    std::string o;
    AppendLine(o, Make(1, 2, 3));
    CHECK(o == "[1,2,3]\n");
    o.clear();
    AppendLine(o, Make(1, 2, 3, 0, 90, 0));
    CHECK(o == "[1,2,3,0,90,0]\n");
    o.clear();
    AppendLine(o, Make(1, 2, 3, 0, 0, 0, 2, 2, 2));
    CHECK(o == "[1,2,3,0,0,0,2,2,2]\n");   // スケールが既定でなければ回転 0 も書く（位置で決まるため）
    o.clear();
    AppendLine(o, Make(-394.4345f, 0.195f, 332.35f));
    CHECK(o == "[-394.4345,0.195,332.35]\n");   // 最短 float（0.019999999552965164 のようなノイズ桁を書かない）
    o.clear();
    AppendLine(o, Make(0.02f, 0, 0));
    CHECK(o == "[0.02,0,0]\n");

    // -0.0 の回転はビット一致でないので省略しない（G4 往復で JSON が変わらないように）
    o.clear();
    AppendLine(o, Make(1, 2, 3, -0.0f, 0, 0));
    CHECK(o == "[1,2,3,-0,0,0]\n");
}

static void Test_ParseLine()
{
    InstanceTRS t;
    CHECK(ParseLine("[1,2,3]", t) && SameBits(t, Make(1, 2, 3)));
    CHECK(ParseLine("  [ 1 , 2 , 3 , 4 , 5 , 6 ]  \r", t) && SameBits(t, Make(1, 2, 3, 4, 5, 6)));
    CHECK(ParseLine("[1,2,3,4,5,6,7,8,9]", t) && SameBits(t, Make(1, 2, 3, 4, 5, 6, 7, 8, 9)));
    CHECK(ParseLine("[1e2,-2.5e-1,0.5]", t) && SameBits(t, Make(100, -0.25f, 0.5f)));
    CHECK(ParseLine("[-0,0,0]", t) && Bits(t.p.x) == Bits(-0.0f));
    // 壊れた行
    CHECK(!ParseLine("", t));
    CHECK(!ParseLine("[]", t));
    CHECK(!ParseLine("[1,2]", t));
    CHECK(!ParseLine("[1,2,3,4]", t));
    CHECK(!ParseLine("[1,2,3,4,5,6,7]", t));
    CHECK(!ParseLine("[1,2,3,4,5,6,7,8,9,10]", t));
    CHECK(!ParseLine("[1,2,x]", t));
    CHECK(!ParseLine("[1,2,3", t));
    CHECK(!ParseLine("1,2,3", t));
    CHECK(!ParseLine("[1,2,3] x", t));
    CHECK(!ParseLine("[1,2,3,]", t));
    CHECK(!ParseLine("[1,,3]", t));
    CHECK(!ParseLine("[nan,0,0]", t));
    CHECK(!ParseLine("[inf,0,0]", t));
    CHECK(!ParseLine("[1e99,0,0]", t));   // float に収まらない
}

static void Test_ParseSidecarBroken()
{
    // 壊れた行は読み飛ばし、以降は読む。空行は無視。
    const std::string text = "[1,2,3]\n\n[oops]\n   \n[4,5,6,0,45,0]\n[7,8]\n[9,9,9]";   // 最後の行は改行なし
    ParseReport rep;
    auto s = ParseSidecar(text, &rep);
    CHECK(s->Count() == 3);
    CHECK(rep.ok == 3 && rep.bad == 2 && rep.lines == 5);
    CHECK(rep.badLineNumbers.size() == 2 && rep.badLineNumbers[0] == 3 && rep.badLineNumbers[1] == 6);
    CHECK(SameBits(s->items[1], Make(4, 5, 6, 0, 45, 0)));
    CHECK(SameBits(s->items[2], Make(9, 9, 9)));
    // CRLF でも読める
    auto s2 = ParseSidecar("[1,2,3]\r\n[4,5,6]\r\n", &rep);
    CHECK(s2->Count() == 2 && rep.bad == 0);
    // 空
    auto s3 = ParseSidecar("", &rep);
    CHECK(s3->Count() == 0 && rep.lines == 0);
}

static void Test_RoundTripRandom()
{
    // ランダムな float（極小・巨大・-0 を含む）を書いて読んでビット一致
    std::mt19937 rng(12345);
    auto rf = [&]() -> f32 {
        const u32 kind = rng() % 6;
        if (kind == 0) return 0.0f;
        if (kind == 1) return -0.0f;
        if (kind == 2) return 1.0f;
        if (kind == 3) { u32 b = rng(); f32 f; std::memcpy(&f, &b, 4); if (!std::isfinite(f)) return 0.5f; return f; }
        return std::uniform_real_distribution<f32>(-1000.0f, 1000.0f)(rng);
    };
    std::vector<InstanceTRS> v;
    for (int i = 0; i < 20000; ++i)
    {
        InstanceTRS t;
        t.p = {rf(), rf(), rf()};
        if (rng() % 3) t.r = {rf(), rf(), rf()};
        if (rng() % 3 == 0) t.s = {rf(), rf(), rf()};
        v.push_back(t);
    }
    auto set = NewSet(std::vector<InstanceTRS>(v));
    const std::string text = FormatSidecar(*set);
    ParseReport rep;
    auto back = ParseSidecar(text, &rep);
    CHECK(rep.bad == 0 && back->Count() == set->Count());
    bool all = back->Count() == set->Count();
    for (u32 i = 0; all && i < back->Count(); ++i) all = SameBits(back->items[i], v[i]);
    CHECK(all);
    // 書き直してバイト一致（固定点）
    CHECK(FormatSidecar(*back) == text);
}

static void Test_MatrixSameAsTransform()
{
    // LocalMatrix は Transform::GetWorldMatrix と同じ（描画の world がエンティティのときと一致する前提）
    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> d(-180.0f, 180.0f);
    bool allEq = true;
    for (int i = 0; i < 2000; ++i)
    {
        InstanceTRS t = Make(d(rng), d(rng), d(rng), d(rng), d(rng), d(rng), 0.5f + std::fabs(d(rng)) * 0.01f, 1.0f, 2.0f);
        Transform tr;
        tr.position = t.p; tr.rotation = t.r; tr.scale = t.s;
        DirectX::XMFLOAT4X4 a, b;
        DirectX::XMStoreFloat4x4(&a, LocalMatrix(t));
        DirectX::XMStoreFloat4x4(&b, tr.GetWorldMatrix());
        allEq = allEq && std::memcmp(&a, &b, sizeof(a)) == 0;
    }
    CHECK(allEq);
}

static void Test_CopyOnWrite()
{
    auto a = NewSet({Make(1, 2, 3), Make(4, 5, 6)});
    auto b = CloneSet(*a);
    CHECK(a->id != b->id);
    b->items.push_back(Make(7, 8, 9));
    CHECK(a->Count() == 2 && b->Count() == 3);
    ComputeBounds(*b);
    CHECK(b->boundsMin.x == 1 && b->boundsMax.x == 7);
    CHECK(!a->boundsValid);
}

static entt::entity AddGroup(Scene& s, const char* name, InstanceSetPtr set, uint64_t guid)
{
    auto& reg = s.GetRegistry();
    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{ name });
    reg.emplace<Transform>(e);
    reg.emplace<EntityGuid>(e, EntityGuid{ guid });
    reg.emplace<InstanceGroup>(e, InstanceGroup{ std::move(set) });
    return e;
}

static std::vector<InstanceTRS> SampleItems(int n, u32 seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> d(-50.0f, 50.0f);
    std::vector<InstanceTRS> v;
    for (int i = 0; i < n; ++i)
    {
        InstanceTRS t = Make(d(rng), d(rng), d(rng));
        if (i % 3 == 1) t.r = {0, d(rng) * 3.0f, 0};
        if (i % 7 == 2) t.s = {2, 2, 2};
        v.push_back(t);
    }
    return v;
}

static void Test_SceneSaveLoad()
{
    const fs::path dir = fs::temp_directory_path() / "dx12_instgroup_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::string scenePath = (dir / "town.json").string();
    const std::string assets = dir.string();

    Scene a;
    auto setA = NewSet(SampleItems(300, 1));
    auto setB = NewSet(SampleItems(5, 2));
    const uint64_t gA = 0x00000000000000a1ull, gB = 0x00000000000000b2ull;
    AddGroup(a, "GroupA", setA, gA);
    AddGroup(a, "GroupB", setB, gB);

    CHECK(SceneSerializer::Save(a, scenePath, assets));
    const fs::path sideA = fs::path(SidecarPathFor(scenePath, "00000000000000a1"));
    const fs::path sideB = fs::path(SidecarPathFor(scenePath, "00000000000000b2"));
    CHECK(fs::exists(sideA) && fs::exists(sideB));
    CHECK(sideA.parent_path().filename() == "town.inst");
    CHECK(ReadAll(sideA) == FormatSidecar(*setA));

    // シーン本体には個数だけ（座標は入らない・メモリ用の mem も入らない）
    const std::string scene1 = ReadAll(scenePath);
    CHECK(scene1.find("\"instanceGroup\":{\"count\":300}") != std::string::npos);
    CHECK(scene1.find("\"mem\"") == std::string::npos);

    // 読み込み: 実体が一致
    Scene b;
    CHECK(SceneSerializer::Load(b, scenePath, assets));
    {
        int found = 0;
        for (auto [e, g, n] : b.GetRegistry().view<InstanceGroup, NameTag>().each())
        {
            ++found;
            CHECK(g._set != nullptr);
            const auto& want = (n.name == "GroupA") ? *setA : *setB;
            CHECK(g._set && g._set->Count() == want.Count());
            bool eq = g._set && g._set->Count() == want.Count();
            for (u32 i = 0; eq && i < want.Count(); ++i) eq = SameBits(g._set->items[i], want.items[i]);
            CHECK(eq);
        }
        CHECK(found == 2);
    }

    // 再保存: シーンもサイドカーもバイト一致（G6）。内容が同じサイドカーには触らない（更新時刻が動かない）
    const auto mtimeA = fs::last_write_time(sideA);
    CHECK(SceneSerializer::Save(b, scenePath, assets));
    CHECK(ReadAll(scenePath) == scene1);
    CHECK(ReadAll(sideA) == FormatSidecar(*setA));
    CHECK(fs::last_write_time(sideA) == mtimeA);

    // 孤児の掃除: グループを消して保存すると、そのサイドカーも消える。他のファイルには触らない
    WriteAll(dir / "town.inst" / "notes.txt", "keep me");
    {
        auto& reg = b.GetRegistry();
        entt::entity victim = entt::null;
        for (auto [e, n] : reg.view<NameTag>().each()) if (n.name == "GroupB") victim = e;
        CHECK(victim != entt::null);
        reg.destroy(victim);
    }
    CHECK(SceneSerializer::Save(b, scenePath, assets));
    CHECK(fs::exists(sideA) && !fs::exists(sideB));
    CHECK(fs::exists(dir / "town.inst" / "notes.txt"));

    // 全グループを消して保存 → .jsonl は消えるが notes.txt があるのでフォルダは残る。消してから保存し直すとフォルダごと消える
    {
        auto& reg = b.GetRegistry();
        std::vector<entt::entity> v;
        for (auto [e, g] : reg.view<InstanceGroup>().each()) v.push_back(e);
        for (auto e : v) reg.destroy(e);
    }
    CHECK(SceneSerializer::Save(b, scenePath, assets));
    CHECK(!fs::exists(sideA));
    fs::remove(dir / "town.inst" / "notes.txt", ec);
    CHECK(SceneSerializer::Save(b, scenePath, assets));
    CHECK(!fs::exists(dir / "town.inst"));

    fs::remove_all(dir, ec);
}

static void Test_BrokenSidecarOnLoad()
{
    const fs::path dir = fs::temp_directory_path() / "dx12_instgroup_test2";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::string scenePath = (dir / "s.json").string();
    Scene a;
    AddGroup(a, "G", NewSet(SampleItems(10, 3)), 0x1234ull);
    CHECK(SceneSerializer::Save(a, scenePath, dir.string()));
    const fs::path side = fs::path(SidecarPathFor(scenePath, "0000000000001234"));
    CHECK(fs::exists(side));

    // 壊れた行を混ぜる → 読めた行だけ（9 行）
    std::string text = ReadAll(side);
    text.insert(text.find('\n') + 1, "[oops]\n");
    WriteAll(side, text);
    {
        Scene b;
        CHECK(SceneSerializer::Load(b, scenePath, dir.string()));
        size_t n = 0;
        for (auto [e, g] : b.GetRegistry().view<InstanceGroup>().each()) n = g._set ? g._set->Count() : 99999;
        CHECK(n == 10);   // [oops] は 1 行だけ。元の 10 行はすべて残る
    }
    // サイドカーが無い → 空のグループ（落ちない）
    fs::remove(side, ec);
    {
        Scene b;
        CHECK(SceneSerializer::Load(b, scenePath, dir.string()));
        size_t groups = 0, n = 0;
        for (auto [e, g] : b.GetRegistry().view<InstanceGroup>().each()) { ++groups; n = g._set ? g._set->Count() : 99999; }
        CHECK(groups == 1 && n == 0);
    }
    fs::remove_all(dir, ec);
}

static void Test_SnapshotAndDuplicateShareSet()
{
    StoreClearForTests();
    Scene a;
    auto set = NewSet(SampleItems(1000, 9));
    const entt::entity ea = AddGroup(a, "G", set, 0x77ull);

    // Play のスナップショット: 実体を共有し（ポインタ同一）、配列を JSON に書かない
    const std::string snap = SceneSerializer::SaveToString(a, "");
    {
        // 配列を JSON に書かない: 個数が 100 倍違っても文字列の長さはほぼ同じ（mem の番号の桁数ぶんだけ）
        Scene sceneSmall;
        AddGroup(sceneSmall, "G", NewSet(SampleItems(10, 9)), 0x77ull);
        const std::string snapSmall = SceneSerializer::SaveToString(sceneSmall, "");
        CHECK(snap.size() < snapSmall.size() + 16);
    }
    Scene b;
    CHECK(SceneSerializer::LoadFromString(b, snap, ""));
    {
        const InstanceSet* got = nullptr;
        for (auto [e, g] : b.GetRegistry().view<InstanceGroup>().each()) got = g._set.get();
        CHECK(got == set.get());
    }
    // 元の実体を差し替えても（コピーオンライト）スナップショットは古い実体のまま
    {
        auto& g = a.GetRegistry().get<InstanceGroup>(ea);
        auto n = CloneSet(*g._set);
        n->items.pop_back();
        g._set = n;
    }
    set.reset();   // 台帳が強い参照を持つので、手元を離してもスナップショットは復元できる
    Scene c;
    CHECK(SceneSerializer::LoadFromString(c, snap, ""));
    {
        u32 cnt = 0;
        for (auto [e, g] : c.GetRegistry().view<InstanceGroup>().each()) cnt = g._set ? g._set->Count() : 0;
        CHECK(cnt == 1000);
    }

    // 複製: 実体を共有（新しい guid・名前は連番）
    const entt::entity dup = SceneSerializer::DuplicateEntity(a, ea, "");
    CHECK(dup != entt::null);
    if (dup != entt::null)
    {
        const auto* ga = a.GetRegistry().try_get<InstanceGroup>(ea);
        const auto* gd = a.GetRegistry().try_get<InstanceGroup>(dup);
        CHECK(ga && gd && ga->_set && gd->_set && ga->_set.get() == gd->_set.get());
    }
}

int main()
{
    Test_LineForms();
    Test_ParseLine();
    Test_ParseSidecarBroken();
    Test_RoundTripRandom();
    Test_MatrixSameAsTransform();
    Test_CopyOnWrite();
    Test_SceneSaveLoad();
    Test_BrokenSidecarOnLoad();
    Test_SnapshotAndDuplicateShareSet();

    std::printf("InstanceGroupTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
