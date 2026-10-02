// シーンの分割保存（docs/SCENE_FORMAT_DESIGN.md §4.3）のテスト。GPU/D3D12 デバイス不要。
//
// 押さえるもの:
//   seq        … 並び順の印の符号化・復号（壊れた入力を弾く）
//   割り当て    … 入れ物（空の親）の子が個別にセルへ・サブツリーは分断しない・境界・ライト/カメラ/印は foo.json・分割しない設定
//   往復        … Split → Merge で entities の並びまで一致（seq が無い / 壊れていても読める・重複や欠けは弾く）
//   Scene       … 分割 ON で Save → Load → SaveToString が分割前とバイト一致（P1 のユニット版）・
//                 保存し直しでファイルが変わらない・1 体動かすと書き換わるファイルが 1〜2 個・
//                 空になったセルのファイルを消す・分割を切ると 1 ファイルへ戻る・セルファイルは単独で開かない
//   参照の書き換え … RewriteAssetPathRefsInFiles がセルファイルにも効く（開いているシーンのセルは飛ばす）

#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/SceneFormatV2.h"
#include "scene/ScenePartition.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る

#include <entt/entt.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

using namespace dx12e;
using json = nlohmann::json;
namespace sp = dx12e::scenepart;
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
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
static void WriteAll(const fs::path& p, const std::string& s)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

static json Ent(const char* guid, const char* name, double x, double z, const char* parentGuid = nullptr, bool spatial = true)
{
    json e = {{"guid", guid}, {"name", name}, {"transform", {{"position", json::array({x, 0.0, z})}}}};
    if (spatial) e["boxCollider"] = json::object();
    if (parentGuid) e["parentGuid"] = parentGuid;
    return e;
}

// ---------------------------------------------------------------------------
static void Test_Seq()
{
    CHECK(sp::EncodeSeq({}) == "");
    CHECK(sp::EncodeSeq({0}) == "0");
    CHECK(sp::EncodeSeq({0, 1, 2, 10, 15, 16}) == "0-2,10,15-16");
    std::vector<int> v;
    CHECK(sp::DecodeSeq("0-2,10,15-16", 6, 20, v));
    CHECK((v == std::vector<int>{0, 1, 2, 10, 15, 16}));
    CHECK(sp::DecodeSeq("", 0, 5, v) && v.empty());
    CHECK(!sp::DecodeSeq("0-2", 4, 20, v));        // 個数が合わない
    CHECK(!sp::DecodeSeq("0-2,", 3, 20, v));       // 末尾のカンマ
    CHECK(!sp::DecodeSeq("a", 1, 20, v));
    CHECK(!sp::DecodeSeq("5-3", 3, 20, v));        // 逆順
    CHECK(!sp::DecodeSeq("25", 1, 20, v));         // 範囲外
    CHECK(!sp::DecodeSeq("0-2,x", 4, 20, v));
    // 往復（ランダムな増加列）
    std::mt19937 rng(7);
    for (int iter = 0; iter < 200; ++iter)
    {
        std::vector<int> src;
        int cur = 0;
        for (int i = 0; i < 50; ++i) { cur += 1 + (rng() % 3 == 0 ? static_cast<int>(rng() % 5) : 0); src.push_back(cur); }
        std::vector<int> back;
        CHECK(sp::DecodeSeq(sp::EncodeSeq(src), src.size(), 1000, back) && back == src);
    }
}

// ---------------------------------------------------------------------------
static void Test_Assign()
{
    // 入れ物（名前・guid・transform だけ）は foo.json に残り、子が個別にセルへ入る
    json ents = json::array();
    ents.push_back(Ent("g0", "Container", 100, 100, nullptr, /*spatial=*/false));      // 0 入れ物（位置が離れていても子の位置で割り当てる）
    ents.push_back(Ent("a", "A", -100, 10, "g0"));                                     // 1: ワールド (0,110) → セル (0,1)
    ents.push_back(Ent("b", "B", -164.5, -100, "g0"));                                 // 2: ワールド (-64.5,0) → セル (-2,0)
    ents.push_back(Ent("b1", "B_child", 5000, 5000, "b"));                             // 3: B のサブツリー（B と同じセル）
    ents.push_back(Ent("c", "C", 63.99, 0));                                           // 4: セル (0,0)
    ents.push_back(Ent("d", "D", 64.0, -0.01));                                        // 5: セル (1,-1)
    ents.push_back(Ent("light", "Light", 10, 10));                                     // 6: ライトを持つ → foo.json
    ents[6]["pointLight"] = json::object();
    ents.push_back(Ent("cam", "Cam", 10, 10, nullptr, false));                         // 7: 空間物ではない → foo.json
    ents[7]["camera"] = json::object();
    ents.push_back(Ent("pin", "Pinned", 10, 10));                                      // 8: 印 → foo.json
    ents[8]["partition"] = "root";
    ents.push_back(Ent("lua", "Mgr", 10, 10));                                         // 9: スクリプト付き → foo.json
    ents[9]["luaScript"] = json::object();
    ents.push_back(Ent("orph", "Orphan", 200, 200, "missing-guid"));                   // 10: 親が見つからない → ルート扱いでセルへ

    const sp::Assignment a = sp::AssignCells(ents, 64.0);
    auto cellOf = [&](int idx) -> const sp::CellKey* {
        for (const auto& c : a.cells) for (int i : c.second) if (i == idx) return &c.first;
        return nullptr;
    };
    CHECK((a.rootIdx == std::vector<int>{0, 6, 7, 8, 9}));
    CHECK(cellOf(1) && *cellOf(1) == (sp::CellKey{0, 1}));
    CHECK(cellOf(2) && *cellOf(2) == (sp::CellKey{-2, 0}));
    CHECK(cellOf(3) && cellOf(2) && *cellOf(3) == *cellOf(2));   // 子はサブツリーごと
    CHECK(cellOf(4) && *cellOf(4) == (sp::CellKey{0, 0}));
    CHECK(cellOf(5) && *cellOf(5) == (sp::CellKey{1, -1}));
    CHECK(cellOf(10) && *cellOf(10) == (sp::CellKey{3, 3}));
    // セルは (x,z) 昇順・各セルの添字は昇順
    for (size_t i = 1; i < a.cells.size(); ++i) CHECK(a.cells[i - 1].first < a.cells[i].first);
    for (const auto& c : a.cells) for (size_t i = 1; i < c.second.size(); ++i) CHECK(c.second[i - 1] < c.second[i]);
    // 全エンティティがちょうど 1 回現れる
    size_t total = a.rootIdx.size();
    for (const auto& c : a.cells) total += c.second.size();
    CHECK(total == ents.size());

    // 分割しない設定（0）は全部 foo.json
    const sp::Assignment z = sp::AssignCells(ents, 0.0);
    CHECK(z.cells.empty() && z.rootIdx.size() == ents.size());

    // 親の輪（壊れた入力）でも全員が割り当てられ、無限ループしない
    json cyc = json::array();
    cyc.push_back(Ent("x", "X", 0, 0, "y"));
    cyc.push_back(Ent("y", "Y", 0, 0, "x"));
    cyc.push_back(Ent("r", "R", 0, 0));
    const sp::Assignment ca = sp::AssignCells(cyc, 64.0);
    size_t ct = ca.rootIdx.size();
    for (const auto& c : ca.cells) ct += c.second.size();
    CHECK(ct == 3);

    // 入れ物の回転・スケールがワールド位置に効く（Y 軸 90° 回転した入れ物の中の +X は -Z へ）
    json rot = json::array();
    rot.push_back({{"guid", "g"}, {"name", "G"}, {"transform", {{"position", json::array({0, 0, 0})}, {"rotation", json::array({0, 90, 0})}, {"scale", json::array({10, 10, 10})}}}});
    rot.push_back(Ent("k", "K", 10, 0, "g"));   // ローカル (10,0,0) → ×10 → 回転 → ワールド (0,0,-100)
    const sp::Assignment ra = sp::AssignCells(rot, 64.0);
    CHECK(ra.cells.size() == 1 && ra.cells[0].first == (sp::CellKey{0, -2}));
}

// ---------------------------------------------------------------------------
static json MakeSampleEntities()
{
    json ents = json::array();
    ents.push_back(Ent("root0", "Root", 0, 0, nullptr, false));
    int n = 0;
    for (int ix = -3; ix <= 3; ++ix)
        for (int iz = -2; iz <= 2; ++iz)
        {
            const std::string g = "e" + std::to_string(n), nm = "E" + std::to_string(n);
            ents.push_back(Ent(g.c_str(), nm.c_str(), ix * 70.0 + 3.0, iz * 70.0 + 5.0, "root0"));
            ++n;
        }
    ents.push_back(Ent("li", "Light", 0, 0)); ents.back()["pointLight"] = json::object();
    return ents;
}

static void Test_SplitMergeRoundtrip()
{
    json root = {{"version", 2}, {"postProcess", {{"exposure", 1.5}}}, {"partition", {{"cellSize", 64}}}, {"entities", MakeSampleEntities()}};
    const json before = root["entities"];

    sp::SplitOutput out;
    sp::SplitAndDump(json(root), 64.0, out);
    CHECK(out.parts.size() == 35);   // 7 x 5 セル
    CHECK(!out.rootText.empty() && out.rootText.back() == '\n' && out.rootText.find('\r') == std::string::npos);

    // foo.json の本文: 正しい JSON・parts は 1 要素 1 行・設定はそのまま
    const json rootDoc = json::parse(out.rootText);
    CHECK(rootDoc.at("version") == 2);
    CHECK(rootDoc.at("partition").at("cellSize") == 64);
    CHECK(rootDoc.at("parts").size() == 35);
    CHECK(rootDoc.at("entities").size() == 2);   // 入れ物とライトだけ
    size_t partsLines = 0;
    for (size_t p = out.rootText.find("\n    {\"bounds\""); p != std::string::npos; p = out.rootText.find("\n    {\"bounds\"", p + 1)) ++partsLines;
    CHECK(partsLines == 35);
    CHECK(rootDoc.at("parts")[0].at("file").is_string() && rootDoc.at("parts")[0].at("count") == 1);
    for (const auto& pt : out.parts)
    {
        const json d = json::parse(pt.text);
        CHECK(d.at("version") == 2 && d.at("seq").is_string() && d.at("entities").size() == pt.count);
        size_t lines = 0;
        for (char c : pt.text) if (c == '\n') ++lines;
        CHECK(lines == pt.count + 6);   // { version, seq, "entities": [ ... ] }（1 体 1 行）
    }

    // 統合: 並びまで元と一致
    auto reader = [&](const std::string& name, std::string& bytes) {
        for (const auto& pt : out.parts) if (pt.name == name) { bytes = pt.text; return true; }
        return false;
    };
    {
        json merged = rootDoc;
        sp::MergeStats ms;
        std::string err;
        CHECK(sp::MergeParts(merged, reader, ms, err));
        CHECK(ms.seqUsed && ms.files == 35);
        CHECK(!merged.contains("parts"));
        CHECK(merged.at("entities") == before);
        CHECK(merged.at("postProcess") == root.at("postProcess"));
    }
    // seq が無い（手書きのセル）→ foo.json → セルの順につなぐ。全員いる
    {
        json merged = rootDoc;
        sp::MergeStats ms;
        std::string err;
        auto noSeq = [&](const std::string& name, std::string& bytes) {
            if (!reader(name, bytes)) return false;
            json d = json::parse(bytes);
            d.erase("seq");
            bytes = d.dump();
            return true;
        };
        CHECK(sp::MergeParts(merged, noSeq, ms, err));
        CHECK(!ms.seqUsed);
        CHECK(merged.at("entities").size() == before.size());
        CHECK(merged.at("entities")[0].at("name") == "Root");   // foo.json のエンティティが先頭
    }
    // 重複した seq は壊れているので、つなぎ順にする（落ちない・全員いる）
    {
        json merged = rootDoc;
        sp::MergeStats ms;
        std::string err;
        auto dup = [&](const std::string& name, std::string& bytes) {
            if (!reader(name, bytes)) return false;
            json d = json::parse(bytes);
            d["seq"] = "0";
            if (d.at("entities").size() > 1) d["seq"] = "0-" + std::to_string(d.at("entities").size() - 1);
            bytes = d.dump();
            return true;
        };
        CHECK(sp::MergeParts(merged, dup, ms, err));
        CHECK(!ms.seqUsed && merged.at("entities").size() == before.size());
    }
    // 読めないセル / 壊れたセル / 不正な名前は失敗（黙って欠けたまま開かない）
    {
        json merged = rootDoc;
        sp::MergeStats ms;
        std::string err;
        CHECK(!sp::MergeParts(merged, [&](const std::string& n, std::string& b) { return n != "cell_0_0.json" && reader(n, b); }, ms, err));
        CHECK(err.find("cell_0_0.json") != std::string::npos);
        json merged2 = rootDoc;
        CHECK(!sp::MergeParts(merged2, [&](const std::string& n, std::string& b) { if (!reader(n, b)) return false; if (n == "cell_1_1.json") b = "{oops"; return true; }, ms, err));
        json merged3 = rootDoc;
        merged3["parts"][0]["file"] = "../evil.json";
        CHECK(!sp::MergeParts(merged3, reader, ms, err));
    }
    CHECK(sp::IsSafePartName("cell_-1_2.json") && !sp::IsSafePartName("a/b.json") && !sp::IsSafePartName("..") && !sp::IsSafePartName("c:x.json"));

    // 分割しない設定・割り当てが空なら通常の v2 と同じ本文（parts 無し）
    sp::SplitOutput none;
    sp::SplitAndDump(json(root), 0.0, none);
    CHECK(none.parts.empty() && none.rootText == scenefmt::DumpSceneV2(root));
}

// ---------------------------------------------------------------------------
static void BuildPartitionScene(Scene& s)
{
    auto& reg = s.GetRegistry();
    auto make = [&](const std::string& name, DirectX::XMFLOAT3 pos, entt::entity parent) {
        entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{ name });
        Transform t;
        t.position = pos;
        t.parent = parent;
        reg.emplace<Transform>(e, t);
        return e;
    };
    const entt::entity group = make("Group", { 0, 0, 0 }, entt::null);   // 入れ物
    const entt::entity group2 = make("Sub", { 10, 0, 0 }, group);        // 入れ物の中の入れ物
    std::mt19937 rng(42);
    for (int i = 0; i < 300; ++i)
    {
        const float x = static_cast<float>(static_cast<int>(rng() % 600) - 300) + 0.25f;
        const float z = static_cast<float>(static_cast<int>(rng() % 400) - 200) + 0.5f;
        const entt::entity e = make("Box" + std::to_string(i), { x, 0.019999999552965164f * i, z }, (i % 2) ? group : group2);
        BoxCollider bc; bc.halfExtents = { 0.5f, 0.5f, 0.5f };
        reg.emplace<BoxCollider>(e, bc);
        if (i % 3 == 0) { RigidBody rb; rb.motionType = MotionType::Static; rb.mass = 0; rb.friction = 0.8f; rb.useGravity = false; reg.emplace<RigidBody>(e, rb); }
        if (i % 10 == 0)
        {
            const entt::entity ch = make("BoxChild" + std::to_string(i), { 0, 1, 0 }, e);
            BoxCollider cb; reg.emplace<BoxCollider>(ch, cb);
        }
    }
    const entt::entity sun = make("Sun", { 0, 50, 0 }, entt::null);
    reg.emplace<DirectionalLight>(sun, DirectionalLight{});
    for (int i = 0; i < 5; ++i)
    {
        const entt::entity l = make("Lamp" + std::to_string(i), { i * 90.0f - 180.0f, 3, 7 }, entt::null);
        PointLight pl; pl.intensity = 3.0f; reg.emplace<PointLight>(l, pl);
    }
}

static int CountFiles(const fs::path& dir)
{
    int n = 0;
    std::error_code ec;
    if (fs::is_directory(dir, ec)) for (const auto& e : fs::directory_iterator(dir, ec)) if (e.is_regular_file(ec)) ++n;
    return n;
}

static void Test_SceneSaveLoad()
{
    const fs::path dir = fs::temp_directory_path() / "dx12_scene_partition_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::string assets = (dir / "assets").string() + "/";
    const fs::path file = dir / "assets" / "scenes" / "big.json";
    const fs::path parts = dir / "assets" / "scenes" / "big.parts";

    Scene a;
    BuildPartitionScene(a);
    // 基準: 分割ありの設定のままの完全形（partition キーを含む）
    a.SetPartitionCellSize(64.0f);
    const std::string baseline = SceneSerializer::SaveToString(a, assets);

    CHECK(SceneSerializer::Save(a, file.string(), assets));
    const auto rep1 = SceneSerializer::LastSaveReport();
    CHECK(rep1.partitioned && rep1.files > 4 && rep1.written == rep1.files + 1);
    CHECK(fs::is_directory(parts));
    CHECK(CountFiles(parts) == rep1.files);
    const json rootDoc = json::parse(ReadAll(file));
    CHECK(rootDoc.at("partition").at("cellSize") == 64);
    CHECK(rootDoc.at("parts").size() == static_cast<size_t>(rep1.files));
    // 入れ物 2 つ + ライト（太陽・ランプ）だけが foo.json に残る
    CHECK(rootDoc.at("entities").size() == 2 + 1 + 5);

    // P1 のユニット版: 読み込み → SaveToString が分割前とバイト一致
    Scene b;
    CHECK(SceneSerializer::Load(b, file.string(), assets));
    CHECK(b.GetPartitionCellSize() == 64.0f);
    const std::string reloaded = SceneSerializer::SaveToString(b, assets);
    CHECK(reloaded == baseline);
    if (reloaded != baseline) std::printf("  baseline %zu bytes / reloaded %zu bytes\n", baseline.size(), reloaded.size());

    // 保存し直しても何も書き換わらない（内容が同じファイルには触らない）
    CHECK(SceneSerializer::Save(b, file.string(), assets));
    const auto rep2 = SceneSerializer::LastSaveReport();
    CHECK(rep2.written == 0 && rep2.unchanged == rep1.files + 1 && rep2.removed == 0);

    // 1 体だけ同じセルの中で動かす → 書き換わるのは 1 ファイル
    auto& regB = b.GetRegistry();
    entt::entity box = entt::null;
    for (auto [e, nt] : regB.view<NameTag>().each()) if (nt.name == "Box3") box = e;
    CHECK(box != entt::null);
    const float oldX = regB.get<Transform>(box).position.x;
    const float oldY = regB.get<Transform>(box).position.y;
    regB.get<Transform>(box).position.y = oldY + 0.5f;
    CHECK(SceneSerializer::Save(b, file.string(), assets));
    const auto rep3 = SceneSerializer::LastSaveReport();
    CHECK(rep3.written == 1);
    // セルをまたいで動かす → 2 ファイル（消える側と増える側）。foo.json は変わらない（セルの一覧が同じ場合）
    regB.get<Transform>(box).position.x = oldX + 200.0f;
    CHECK(SceneSerializer::Save(b, file.string(), assets));
    const auto rep4 = SceneSerializer::LastSaveReport();
    CHECK(rep4.written >= 1 && rep4.written <= 3);   // 元のセル・新しいセル（新しいセルが無かったときは foo.json の一覧も）
    // 元へ戻してから、読み直しても並びが保たれる
    regB.get<Transform>(box).position.x = oldX;
    regB.get<Transform>(box).position.y = oldY;
    CHECK(SceneSerializer::Save(b, file.string(), assets));
    Scene c;
    CHECK(SceneSerializer::Load(c, file.string(), assets));
    CHECK(SceneSerializer::SaveToString(c, assets) == baseline);

    // 空になったセルのファイルは消える（全員を 1 つのセルの外へ出さず、1 セルぶんだけ空にする）
    {
        const int before = CountFiles(parts);
        auto& regC = c.GetRegistry();
        // 先頭のセルファイルの中身のエンティティ名を調べて、それらを全部削除する
        const json firstPart = json::parse(ReadAll(parts / rootDoc.at("parts")[0].at("file").get<std::string>()));
        std::vector<std::string> names;
        for (const auto& e : firstPart.at("entities")) names.push_back(e.at("name").get<std::string>());
        std::vector<entt::entity> kill;
        for (auto [e, nt] : regC.view<NameTag>().each()) for (const auto& n : names) if (nt.name == n) kill.push_back(e);
        for (auto e : kill) regC.destroy(e);
        CHECK(SceneSerializer::Save(c, file.string(), assets));
        CHECK(SceneSerializer::LastSaveReport().removed == 1);
        CHECK(CountFiles(parts) == before - 1);
    }

    // セルファイルは単独では開かない
    {
        Scene d;
        CHECK(!SceneSerializer::Load(d, (parts / rootDoc.at("parts")[1].at("file").get<std::string>()).string(), assets));
    }

    // ReadSceneFileMerged（--validate / ApplyOverrides の経路）: 補完前の全エンティティ数が合う
    {
        json merged;
        std::string err;
        CHECK(sp::ReadSceneFileMerged(file.string(), merged, err));
        CHECK(!merged.contains("parts"));
    }

    // 分割をやめる → 次の保存で 1 ファイルへ戻る（セルファイルとフォルダが消える）
    c.SetPartitionCellSize(0.0f);
    CHECK(SceneSerializer::Save(c, file.string(), assets));
    CHECK(!fs::exists(parts));
    const json single = json::parse(ReadAll(file));
    CHECK(!single.contains("parts") && !single.contains("partition"));
    CHECK(single.at("entities").size() == c.GetRegistry().view<NameTag>().size());
    Scene e2;
    CHECK(SceneSerializer::Load(e2, file.string(), assets));
    CHECK(e2.GetPartitionCellSize() == 0.0f);
    CHECK(SceneSerializer::SaveToString(e2, assets) == SceneSerializer::SaveToString(c, assets));

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
static void Test_RewriteInParts()
{
    const fs::path dir = fs::temp_directory_path() / "dx12_scene_partition_rewrite_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path assets = dir / "assets";
    sp::SplitOutput out;
    json ents = json::array();
    for (int i = 0; i < 4; ++i)
    {
        json e = Ent(("g" + std::to_string(i)).c_str(), ("E" + std::to_string(i)).c_str(), i * 100.0, 0);
        e["meshRenderer"] = {{"modelPath", "models/a.glb"}};
        ents.push_back(e);
    }
    sp::SplitAndDump(json{{"version", 2}, {"entities", ents}}, 64.0, out);
    CHECK(out.parts.size() == 4);
    const fs::path scene = assets / "scenes" / "s.json";
    CHECK(sp::WriteSplit(scene.string(), out).ok);

    // 開いていないシーン: ルートもセルも書き換わる（セルは v2 の整形のまま）
    const auto r = SceneSerializer::RewriteAssetPathRefsInFiles(assets.string() + "/", "models/a.glb", "models/b.glb");
    CHECK(r.filesChanged == 4 && r.refsChanged == 4);
    for (const auto& pt : out.parts)
    {
        const std::string txt = ReadAll(sp::PartsDirFor(scene.string()) + "/" + pt.name);
        CHECK(txt.find("models/b.glb") != std::string::npos && txt.find("models/a.glb") == std::string::npos);
        CHECK(txt.find("\n  \"entities\": [\n{") != std::string::npos);   // 1 体 1 行のまま
    }
    // 開いているシーン（skip 指定）のセルは飛ばす
    const auto r2 = SceneSerializer::RewriteAssetPathRefsInFiles(assets.string() + "/", "models/b.glb", "models/c.glb", scene.string());
    CHECK(r2.filesChanged == 0);
    fs::remove_all(dir, ec);
}

int main()
{
    Test_Seq();
    Test_Assign();
    Test_SplitMergeRoundtrip();
    Test_SceneSaveLoad();
    Test_RewriteInParts();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
