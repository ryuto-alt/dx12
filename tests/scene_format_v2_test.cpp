// シーンファイル形式 v2（docs/SCENE_FORMAT_DESIGN.md §3）の純関数テスト。GPU/D3D12 デバイス不要。
//
// 押さえるもの:
//   F2  inflate(strip(normalize(e))) == normalize(e)（既定値のままのもの / 違うもの / 部分的に違うもの）
//   最短 float: float32 で正確な double だけが短い表記へ。読み戻すと同じ float32（全 32bit パターンの間引き走査）
//   凍結の既定値表: 埋め込み表が読めて、反射コンポーネントの既定構築と食い違っていない（食い違い = 表を変えてはいけない / v3 を作る合図）
//   整形: version → 設定（dump(2)）→ entities が 1 行 1 体・区切りに空白なし・末尾改行・LF のみ・正しい JSON
//   往復: Scene → SaveToStringV2 → LoadFromString → SaveToString が、v1 完全形の往復とバイト一致（GPU 不要のエンティティ）
//   親参照: parentGuid があれば parent(index) を書かない。guid が無い旧形式は index でも読める
//   互換: v1（version 無し）は従来どおり・v2 の省略形（rigidBody:{} / transform は position だけ）が既定で補完される
//
// 生成モード: `SceneFormatV2Tests --emit-defaults <out.json>` は凍結表の初期値を書き出す（表を作るときだけ使う。
// 表は凍結データなので、出力で既存の表を自動更新しない）。

#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/SceneFormatV2.h"
#include "scene/Entity.h"
#include "ecs/Components.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る

#include <entt/entt.hpp>

#include <cfloat>
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
using json = nlohmann::json;

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

#define CHECK_EQ_JSON(a, b)                                                  \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!((a) == (b))) {                                                 \
            std::printf("FAIL %s:%d: %s != %s\n  left : %s\n  right: %s\n", __FILE__, __LINE__, #a, #b, \
                        (a).dump().c_str(), (b).dump().c_str());             \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// ---- 最短 float ----
static void Test_NormalizeDouble()
{
    // float を double として dump したときのノイズ桁が、本来の短い値に戻る
    CHECK(scenefmt::NormalizeDouble(0.019999999552965164) == 0.02);
    CHECK(scenefmt::NormalizeDouble(0.800000011920929) == 0.8);
    CHECK(scenefmt::NormalizeDouble(0.20000000298023224) == 0.2);
    CHECK(scenefmt::NormalizeDouble(0.009999999776482582) == 0.01);
    // float32 で正確でない double（ユーザーが書いた 0.02 / 0.1 や、1/3）は触らない
    CHECK(scenefmt::NormalizeDouble(0.02) == 0.02);
    CHECK(scenefmt::NormalizeDouble(0.1) == 0.1);
    CHECK(scenefmt::NormalizeDouble(1.0 / 3.0) == 1.0 / 3.0);
    // 整数値・2 の冪・ゼロ・負のゼロ
    CHECK(scenefmt::NormalizeDouble(0.0) == 0.0);
    CHECK(std::signbit(scenefmt::NormalizeDouble(-0.0)));
    CHECK(scenefmt::NormalizeDouble(1.0) == 1.0);
    // 非有限・float 範囲外は触らない
    CHECK(std::isinf(scenefmt::NormalizeDouble(HUGE_VAL)));
    CHECK(scenefmt::NormalizeDouble(1e300) == 1e300);

    // 任意の float32: 正規化後の double を float に戻すと元と同じビット（=読み込み後の値が 1 ビットも変わらない）。
    // かつ JSON を経由（dump → parse）しても同じ。
    std::mt19937 rng(12345);
    int bad = 0, shortened = 0;
    for (int i = 0; i < 300000; ++i)
    {
        std::uint32_t bits = static_cast<std::uint32_t>(rng());
        // 半分は「それっぽい」値（小数 3 桁程度）も混ぜる
        float f;
        if (i % 2) { std::memcpy(&f, &bits, 4); }
        else       { f = static_cast<float>(static_cast<int>(rng() % 2000001) - 1000000) / 1000.0f; }
        if (!std::isfinite(f)) continue;
        const double d = static_cast<double>(f);
        const double n = scenefmt::NormalizeDouble(d);
        const float back = static_cast<float>(n);
        std::uint32_t b0, b1;
        std::memcpy(&b0, &f, 4); std::memcpy(&b1, &back, 4);
        if (b0 != b1) ++bad;
        // JSON 往復
        json j = n;
        const json k = json::parse(j.dump());
        const float back2 = static_cast<float>(k.get<double>());
        std::uint32_t b2; std::memcpy(&b2, &back2, 4);
        if (b0 != b2) ++bad;
        if (n != d) ++shortened;
    }
    CHECK(bad == 0);
    CHECK(shortened > 1000);   // 実際に短縮されている（ノイズ桁が減る）
}

// ---- 凍結の既定値表 ----
static void Test_Table()
{
    const json& t = scenefmt::DefaultsV2();
    CHECK(t.is_object());
    CHECK(t.contains("transform"));
    CHECK(t.contains("rigidBody"));
    if (t.contains("rigidBody"))
    {
        // 静的コライダーの姿（動的の既定構築ではない）。凍結データなので変えるなら v3。
        CHECK(t["rigidBody"].value("motionType", -1) == 0);
        CHECK(t["rigidBody"].value("mass", -1.0) == 0.0);
        CHECK(t["rigidBody"].value("friction", -1.0) == 0.8);
        CHECK(t["rigidBody"].value("useGravity", true) == false);
    }
    CHECK(t.contains("meshCollider"));
    if (t.contains("transform"))
    {
        CHECK(!t["transform"].contains("position"));   // 位置は表に入れない
        const json rot = json::array({0.0, 0.0, 0.0});
        CHECK(t["transform"].value("rotation", json()) == rot);
        const json sc = json::array({1.0, 1.0, 1.0});
        CHECK(t["transform"].value("scale", json()) == sc);
    }
    // 表の値は正規化済み（ノイズ桁を持たない）
    json copy = t;
    scenefmt::NormalizeFloats(copy);
    CHECK(copy == t);
    // 表が空なら以降の strip / inflate が全部素通しになる＝テストが空振りしないよう、一定数のキーを要求する
    CHECK(t.size() >= 20);
}

// 生成モードと同じ出力が、いまの構造体の既定値から作れる（表と食い違えば「構造体の既定値が変わった」合図）。
// ★表は凍結。食い違っても表を直さない（v3 を新設する）。ここは情報として出すだけで失敗にしない。
static void Report_TableDrift()
{
    const json live = json::parse(SceneSerializer::BuildDefaultsTableV2Json());
    const json& frozen = scenefmt::DefaultsV2();
    int drift = 0;
    for (auto it = live.begin(); it != live.end(); ++it)
    {
        if (!frozen.contains(it.key()) || frozen[it.key()] != it.value())
        {
            ++drift;
            std::printf("INFO: 既定値表と現在の構造体の既定値が違います（表は凍結。変えるなら v3 を新設）: %s\n", it.key().c_str());
        }
    }
    std::printf("INFO: 既定値表 %zu キー / 現在の既定構築 %zu キー / 食い違い %d\n", frozen.size(), live.size(), drift);
}

// ---- F2: strip / inflate の往復 ----
static json RandomizeComponent(const json& defaults, std::mt19937& rng, int mode)
{
    // mode 0: 全部既定 / 1: 1 フィールドだけ違う / 2: 全部違う / 3: 一部欠けている
    json c = defaults;
    if (c.empty()) return c;
    int idx = 0;
    for (auto it = c.begin(); it != c.end(); ++it, ++idx)
    {
        auto alter = [&](json& v) {
            if (v.is_boolean()) v = !v.get<bool>();
            else if (v.is_number_integer()) v = v.get<long long>() + 1 + static_cast<int>(rng() % 3);
            else if (v.is_number_float()) v = v.get<double>() + 0.25 * (1 + static_cast<int>(rng() % 4));
            else if (v.is_string()) v = std::string("x") + v.get<std::string>();
            else if (v.is_array() && !v.empty()) { v[0] = v[0].is_number_float() ? json(v[0].get<double>() + 1.5) : json(7); }
        };
        if (mode == 1 && idx == static_cast<int>(rng() % c.size())) alter(it.value());
        if (mode == 2) alter(it.value());
    }
    if (mode == 3)
    {
        std::vector<std::string> drop;
        for (auto it = c.begin(); it != c.end(); ++it) if (rng() % 2) drop.push_back(it.key());
        for (auto& k : drop) c.erase(k);
    }
    return c;
}

static void Test_StripInflateRoundtrip()
{
    const json& table = scenefmt::DefaultsV2();
    std::mt19937 rng(777);
    int cases = 0;
    for (int trial = 0; trial < 2000; ++trial)
    {
        json e = json::object();
        e["name"] = "E" + std::to_string(trial);
        e["guid"] = "0123456789abcdef";
        json tr = json::object();
        tr["position"] = json::array({0.019999999552965164, -394.4345, 332.35});
        tr["rotation"] = (trial % 3 == 0) ? json::array({0.0, 0.0, 0.0}) : json::array({0.0, 90.0, 0.0});
        tr["scale"]    = (trial % 2 == 0) ? json::array({1.0, 1.0, 1.0}) : json::array({2.0, 1.0, 1.0});
        e["transform"] = tr;
        for (auto it = table.begin(); it != table.end(); ++it)
        {
            if (it.key() == "transform" || !it.value().is_object()) continue;
            if (rng() % 3 != 0) continue;   // 全部のキーを毎回付けるとテストが重いだけ
            e[it.key()] = RandomizeComponent(it.value(), rng, static_cast<int>(rng() % 4));
        }
        // 表に無いキー・配列・値がオブジェクトでないキーは触られない
        e["material"] = json{ {"metallic", 1.0}, {"roughness", 0.1} };
        e["meshRenderer"] = json{ {"modelPath", "models/a.glb"} };
        e["tags"] = json::array({"a", "b"});
        e["convexHullCollider"] = true;

        json norm = e;
        scenefmt::NormalizeFloats(norm);

        json work = norm;
        scenefmt::StripDefaults(work, table);
        // 省略後に消えるはずのものは消えている（サイズが増えない）
        CHECK(work.dump().size() <= norm.dump().size());
        scenefmt::InflateDefaults(work, table);

        // 補完後は「表のフィールドが全部入った完全形」。元が部分的だったもの(mode 3)は表で埋まる＝元より大きくなり得る。
        // 元から全フィールドを持っていたもの（部分欠けでない）は完全に一致する。
        json full = norm;
        scenefmt::InflateDefaults(full, table);
        CHECK(work == full);
        ++cases;
    }
    CHECK(cases == 2000);

    // 既定のままのコンポーネントは {} へ潰れ、キーは残る（存在 = コンポーネントがある）
    json e = json{ {"name", "A"}, {"rigidBody", table.value("rigidBody", json::object())},
                   {"meshCollider", table.value("meshCollider", json::object())} };
    scenefmt::StripDefaults(e, table);
    CHECK(e.contains("rigidBody") && e["rigidBody"].is_object() && e["rigidBody"].empty());
    CHECK(e.contains("meshCollider") && e["meshCollider"].is_object() && e["meshCollider"].empty());

    // 負のゼロは 0.0 と別物（float32 のビットが違う）。省略してはいけない。
    {
        json nz = json{ {"transform", json{ {"rotation", json::array({0.0, -0.0, 0.0})}, {"scale", json::array({1.0, 1.0, 1.0})} }} };
        scenefmt::StripDefaults(nz, table);
        CHECK(nz["transform"].contains("rotation"));
        CHECK(!nz["transform"].contains("scale"));
        scenefmt::InflateDefaults(nz, table);
        const double z = nz["transform"]["rotation"][1].get<double>();
        CHECK(z == 0.0 && std::signbit(z));
        // 整数 0 と浮動小数 0.0 も別物として残す（型を勝手に変えない）
        json iz = json{ {"rigidBody", json{ {"mass", 0} }} };
        scenefmt::StripDefaults(iz, table);
        CHECK(iz["rigidBody"].contains("mass"));
    }

    // 入れ子は再帰しない・配列は丸ごと比較: 要素が 1 つ違えば配列ごと残る
    json e2 = json{ {"transform", json{ {"rotation", json::array({0.0, 0.0, 0.5})}, {"scale", json::array({1.0, 1.0, 1.0})} }} };
    scenefmt::StripDefaults(e2, table);
    CHECK(e2["transform"].contains("rotation"));
    CHECK(!e2["transform"].contains("scale"));
}

// ---- 整形 ----
static void Test_Dump()
{
    json root;
    root["version"] = 1;
    root["postProcess"] = json{ {"exposure", 0.5}, {"bloom", json{ {"enabled", true}, {"threshold", 1.0} }} };
    root["skybox"] = json{ {"envMapPath", "sky/a.hdr"} };
    root["entities"] = json::array();
    for (int i = 0; i < 3; ++i)
    {
        json e = json{ {"guid", "000000000000000" + std::to_string(i + 1)}, {"name", "E" + std::to_string(i)},
                       {"transform", json{ {"position", json::array({i * 1.0, 0.0, 0.0})},
                                           {"rotation", json::array({0.0, 0.0, 0.0})},
                                           {"scale", json::array({1.0, 1.0, 1.0})} }},
                       {"rigidBody", scenefmt::DefaultsV2().value("rigidBody", json::object())} };
        if (i > 0) { e["parentGuid"] = "0000000000000001"; e["parent"] = 0; }
        root["entities"].push_back(e);
    }
    json v2 = root;
    scenefmt::ConvertToV2(v2);
    const std::string text = scenefmt::DumpSceneV2(v2);

    CHECK(!text.empty() && text.back() == '\n');
    CHECK(text.find('\r') == std::string::npos);
    CHECK(text.rfind("{\n  \"version\": 2,\n", 0) == 0);
    const json parsed = json::parse(text);              // 正しい JSON のまま（既存ツールがそのまま読める）
    CHECK(parsed.at("version") == 2);
    CHECK(parsed.at("entities").size() == 3);
    CHECK(parsed.at("postProcess") == v2.at("postProcess"));

    // entities は 1 要素 1 行・区切りに空白なし
    int entityLines = 0;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = text.substr(pos, nl - pos);
        if (line.rfind("{\"", 0) == 0)
        {
            ++entityLines;
            CHECK(line.find(": ") == std::string::npos);
            CHECK(line.find(", ") == std::string::npos);
        }
        pos = nl + 1;
    }
    CHECK(entityLines == 3);

    // parentGuid があれば parent(index) は無い
    CHECK(!parsed["entities"][1].contains("parent"));
    CHECK(parsed["entities"][1].contains("parentGuid"));
    // ルートは parent 自体が無いので影響なし。既定値は省略され、{} は残る
    CHECK(parsed["entities"][0]["rigidBody"].is_object() && parsed["entities"][0]["rigidBody"].empty());
    CHECK(!parsed["entities"][0]["transform"].contains("rotation"));
    CHECK(parsed["entities"][0]["transform"].contains("position"));

    // 空の entities
    json empty = json{ {"version", 1}, {"entities", json::array()} };
    scenefmt::ConvertToV2(empty);
    const std::string t2 = scenefmt::DumpSceneV2(empty);
    CHECK(json::parse(t2).at("entities").empty());

    // ConvertToV2 → InflateScene で、parent 以外は元（正規化済み完全形）に戻る
    json back = json::parse(text);
    CHECK(scenefmt::InflateScene(back));
    json expect = root;
    scenefmt::NormalizeFloats(expect);
    for (auto& e : expect["entities"]) if (e.contains("parentGuid")) e.erase("parent");
    expect["version"] = 2;
    for (auto& e : expect["entities"]) scenefmt::InflateDefaults(e, scenefmt::DefaultsV2());
    CHECK_EQ_JSON(back, expect);
}

static void Test_IsV2()
{
    CHECK(!scenefmt::IsV2(json{ {"entities", json::array()} }));
    CHECK(!scenefmt::IsV2(json{ {"version", 1} }));
    CHECK(scenefmt::IsV2(json{ {"version", 2} }));
    CHECK(scenefmt::IsV2(json{ {"version", 3} }));
    CHECK(!scenefmt::IsV2(json{ {"version", "2"} }));
    CHECK(!scenefmt::IsV2(json::array()));
    // v1 は補完しない
    json v1 = json{ {"version", 1}, {"entities", json::array({ json{ {"name", "A"}, {"rigidBody", json::object()} } })} };
    CHECK(!scenefmt::InflateScene(v1));
    CHECK(v1["entities"][0]["rigidBody"].empty());
}

// ---- Scene を通した往復（GPU 不要のエンティティ: ライト / 物理 / UI / 親子）----
static void BuildSampleScene(Scene& s)
{
    auto& reg = s.GetRegistry();
    auto make = [&](const char* name, DirectX::XMFLOAT3 pos) {
        entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{ name });
        Transform t;
        t.position = pos;
        reg.emplace<Transform>(e, t);
        return e;
    };
    const entt::entity root = make("Root", { 0, 0, 0 });
    entt::entity firstChild = entt::null;
    for (int i = 0; i < 40; ++i)
    {
        const entt::entity e = make(("Child" + std::to_string(i)).c_str(), { 0.1f * i, 0.019999999552965164f * i, -394.4345f });
        if (i == 0) firstChild = e;
        reg.get<Transform>(e).parent = root;
        reg.get<Transform>(e).scale = (i % 2) ? DirectX::XMFLOAT3{ 1, 1, 1 } : DirectX::XMFLOAT3{ 2, 1, 0.5f };
        if (i % 3 == 0) { RigidBody rb; reg.emplace<RigidBody>(e, rb); }
        if (i % 3 == 1) { RigidBody rb; rb.motionType = MotionType::Static; rb.mass = 0; rb.friction = 0.8f; rb.useGravity = false; reg.emplace<RigidBody>(e, rb); }
        if (i % 4 == 0) { MeshCollider mc; reg.emplace<MeshCollider>(e, mc); }
        if (i % 5 == 0) { PointLight pl; pl.intensity = 3.5f; pl.range = 9.0f; reg.emplace<PointLight>(e, pl); }
        if (i % 7 == 0) { BoxCollider bc; bc.halfExtents = { 0.5f, 0.25f, 0.5f }; reg.emplace<BoxCollider>(e, bc); }
    }
    // 孫（2 段目の親子）
    const entt::entity gc = make("Grandchild", { 1, 2, 3 });
    reg.get<Transform>(gc).parent = firstChild;
}

static void Test_SceneRoundtrip()
{
    Scene a;
    BuildSampleScene(a);

    // v1 完全形（Play スナップショットと同じ経路）— 基準
    const std::string v1text = SceneSerializer::SaveToString(a, "");
    CHECK(json::parse(v1text).at("version") == 1);

    // 完全形のまま読み戻して SaveToString（基準の往復。guid は保存で確定済みなので再保存でも同じ）
    Scene b;
    CHECK(SceneSerializer::LoadFromString(b, v1text, ""));
    const std::string v1again = SceneSerializer::SaveToString(b, "");

    // v2 で保存 → 読み込み → SaveToString が v1 の往復と一致する（F1 のユニット版）
    const std::string v2text = SceneSerializer::SaveToStringV2(a, "");
    CHECK(v2text.size() < v1text.size());
    const json v2json = json::parse(v2text);
    CHECK(v2json.at("version") == 2);
    for (const auto& e : v2json.at("entities"))
        if (e.contains("parentGuid")) CHECK(!e.contains("parent"));
    Scene c;
    CHECK(SceneSerializer::LoadFromString(c, v2text, ""));
    const std::string v2again = SceneSerializer::SaveToString(c, "");
    CHECK(v2again == v1again);
    if (v2again != v1again)
        std::printf("  v1 往復: %zu bytes / v2 往復: %zu bytes\n", v1again.size(), v2again.size());

    // v2 → v2 は固定点（保存し直しても同じ文字列。差分が出ない）
    const std::string v2c = SceneSerializer::SaveToStringV2(c, "");
    CHECK(v2c == v2text);
    if (v2c != v2text)
    {
        size_t i = 0;
        while (i < v2c.size() && i < v2text.size() && v2c[i] == v2text[i]) ++i;
        std::printf("  first diff at %zu\n  a: %.200s\n  c: %.200s\n", i, v2text.c_str() + (i > 60 ? i - 60 : 0), v2c.c_str() + (i > 60 ? i - 60 : 0));
    }
}

static void Test_HandWrittenV2()
{
    // 省略形（rigidBody:{} / transform は position だけ）が既定で補完され、親は parentGuid で解決する
    const char* text = R"({"version":2,"entities":[
{"guid":"00000000000000aa","name":"P","transform":{"position":[1,2,3]}},
{"guid":"00000000000000bb","name":"C","parentGuid":"00000000000000aa","rigidBody":{},"meshCollider":{},"transform":{"position":[0,0,0]}}
]})";
    Scene s;
    CHECK(SceneSerializer::LoadFromString(s, text, ""));
    auto& reg = s.GetRegistry();
    entt::entity p = entt::null, c = entt::null;
    for (auto [e, n] : reg.view<NameTag>().each()) { if (n.name == "P") p = e; if (n.name == "C") c = e; }
    CHECK(p != entt::null && c != entt::null);
    if (p != entt::null && c != entt::null)
    {
        CHECK(reg.get<Transform>(c).parent == p);
        const auto& t = reg.get<Transform>(p);
        CHECK(t.scale.x == 1.0f && t.scale.y == 1.0f && t.scale.z == 1.0f);
        CHECK(t.position.x == 1.0f && t.position.y == 2.0f && t.position.z == 3.0f);
        CHECK(reg.all_of<RigidBody>(c));
        CHECK(reg.all_of<MeshCollider>(c));
        // 補完された既定 = 表の値。rigidBody は「静的コライダー」の姿（設計書 §3.1 の rigidBody:{} = 静的）。
        if (reg.all_of<RigidBody>(c))
        {
            const auto& rb = reg.get<RigidBody>(c);
            CHECK(rb.motionType == MotionType::Static);
            CHECK(rb.mass == 0.0f);
            CHECK(rb.friction == 0.8f);
            CHECK(rb.restitution == 0.0f);
            CHECK(!rb.useGravity);
            const RigidBody def{};   // 残りは構造体の既定のまま
            CHECK(rb.linearDamping == def.linearDamping);
            CHECK(rb.angularDamping == def.angularDamping);
        }
    }

    // v1 の index 親（parentGuid なし）は従来どおり読める
    const char* v1 = R"({"version":1,"entities":[
{"name":"P","transform":{"position":[1,2,3],"rotation":[0,0,0],"scale":[1,1,1]}},
{"name":"C","parent":0,"transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}}
]})";
    Scene s1;
    CHECK(SceneSerializer::LoadFromString(s1, v1, ""));
    auto& r1 = s1.GetRegistry();
    entt::entity p1 = entt::null, c1 = entt::null;
    for (auto [e, n] : r1.view<NameTag>().each()) { if (n.name == "P") p1 = e; if (n.name == "C") c1 = e; }
    CHECK(p1 != entt::null && c1 != entt::null);
    if (p1 != entt::null && c1 != entt::null) CHECK(r1.get<Transform>(c1).parent == p1);

    // v1 に省略形は無い: version 無し・{} の rigidBody は T{} 既定のまま（従来挙動）
    const char* v1b = R"({"entities":[{"name":"X","rigidBody":{},"transform":{"position":[0,0,0]}}]})";
    Scene s2;
    CHECK(SceneSerializer::LoadFromString(s2, v1b, ""));
    CHECK(s2.GetRegistry().view<RigidBody>().size() == 1);
}

// ---- ファイル経由（Save / Load / ApplyOverrides / RewriteAssetPathRefsInFiles）----
static std::string ReadAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static void WriteAll(const std::filesystem::path& p, const std::string& s)
{
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

static void Test_SaveLoadFile()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dx12_scene_v2_file_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const std::string assets = (dir / "assets").string() + "/";

    Scene a;
    BuildSampleScene(a);
    const fs::path file = dir / "assets" / "scenes" / "a.json";
    CHECK(SceneSerializer::Save(a, file.string(), assets));

    const std::string onDisk = ReadAll(file);
    CHECK(!onDisk.empty());
    CHECK(onDisk.find('\r') == std::string::npos);   // 改行は LF（バイナリで書く）
    CHECK(onDisk.back() == '\n');
    CHECK(onDisk == SceneSerializer::SaveToStringV2(a, assets));
    CHECK(json::parse(onDisk).at("version") == 2);

    // ディスクから読み直して v1 完全形が一致（Load = ディスクフォールバック経路）
    Scene b;
    CHECK(SceneSerializer::Load(b, file.string(), assets));
    Scene c;
    CHECK(SceneSerializer::LoadFromString(c, SceneSerializer::SaveToString(a, assets), assets));
    CHECK(SceneSerializer::SaveToString(b, assets) == SceneSerializer::SaveToString(c, assets));

    // ApplyOverrides: v2 の省略形（rigidBody:{} / transform は position だけ）を補完してから読む
    WriteAll(dir / "ov.json", R"({"version":2,"entities":[{"name":"Child0","rigidBody":{},"transform":{"position":[9,8,7]}}]})");
    {
        Scene s;
        auto& reg = s.GetRegistry();
        const entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{ "Child0" });
        Transform t; t.rotation = { 1, 2, 3 }; t.scale = { 5, 5, 5 };
        reg.emplace<Transform>(e, t);
        RigidBody rb; rb.mass = 42.0f;
        reg.emplace<RigidBody>(e, rb);
        CHECK(SceneSerializer::ApplyOverrides(s, (dir / "ov.json").string(), assets));
        const auto& tt = reg.get<Transform>(e);
        CHECK(tt.position.x == 9.0f && tt.position.y == 8.0f && tt.position.z == 7.0f);
        CHECK(tt.rotation.x == 0.0f && tt.scale.x == 1.0f);   // 省略 = 表の既定（回転 0 / スケール 1）
        CHECK(reg.get<RigidBody>(e).motionType == MotionType::Static);   // 省略した rigidBody:{} = 静的コライダー
        CHECK(reg.get<RigidBody>(e).mass == 0.0f);
    }
    // position が無い transform でも落ちない（const operator[] の欠損キーは未定義動作だった）
    WriteAll(dir / "ov2.json", R"({"version":2,"entities":[{"name":"Child0","transform":{"rotation":[0,90,0]}}]})");
    {
        Scene s;
        auto& reg = s.GetRegistry();
        const entt::entity e = reg.create();
        reg.emplace<NameTag>(e, NameTag{ "Child0" });
        Transform t; t.position = { 1, 2, 3 };
        reg.emplace<Transform>(e, t);
        CHECK(SceneSerializer::ApplyOverrides(s, (dir / "ov2.json").string(), assets));
        CHECK(reg.get<Transform>(e).position.x == 1.0f);   // position が無ければ触らない
        CHECK(reg.get<Transform>(e).rotation.y == 90.0f);
    }

    fs::remove_all(dir, ec);
}

static void Test_RewriteAssetPathRefsInFiles()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dx12_scene_v2_rewrite_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path assets = dir / "assets";

    Scene a;
    BuildSampleScene(a);
    // モデル参照を持つ v2 シーン（GPU 不要の手書き）と、v1 のシーン / プレハブ
    const std::string v2scene =
        "{\n  \"version\": 2,\n  \"shadows\": true,\n  \"entities\": [\n"
        "{\"guid\":\"0000000000000001\",\"name\":\"A\",\"transform\":{\"position\":[0.02,0,0]}},\n"
        "{\"guid\":\"0000000000000002\",\"meshRenderer\":{\"modelPath\":\"models/a.glb\"},\"name\":\"B\",\"parentGuid\":\"0000000000000001\",\"rigidBody\":{},\"transform\":{\"position\":[1,2,3]}}\n"
        "  ]\n}\n";
    WriteAll(assets / "scenes" / "v2.json", v2scene);
    WriteAll(assets / "scenes" / "v1.json", R"({"version":1,"entities":[{"name":"B","meshRenderer":{"modelPath":"models/a.glb"},"transform":{"position":[0,0,0],"rotation":[0,0,0],"scale":[1,1,1]}}]})");
    WriteAll(assets / "p.prefab", R"({"version":1,"entities":[{"name":"P","meshRenderer":{"modelPath":"models/a.glb"}}]})");

    const auto r = SceneSerializer::RewriteAssetPathRefsInFiles(assets.string() + "/", "models/a.glb", "models/b.glb");
    CHECK(r.filesChanged == 3);
    CHECK(r.refsChanged == 3);
    CHECK(r.failed.empty());

    // v2 は v2 の整形のまま（dump(2) で 1 体 45 行に崩さない）
    const std::string out = ReadAll(assets / "scenes" / "v2.json");
    const std::string expect = [&]() {
        std::string e = v2scene;
        const std::string from = "models/a.glb", to = "models/b.glb";
        e.replace(e.find(from), from.size(), to);
        return e;
    }();
    CHECK(out == expect);
    CHECK(out.find("models/b.glb") != std::string::npos && out.find("models/a.glb") == std::string::npos);
    // v1 / .prefab は従来どおり（2 スペース整形・version 1 のまま）
    const std::string outV1 = ReadAll(assets / "scenes" / "v1.json");
    CHECK(outV1.find("\n  \"entities\"") != std::string::npos);
    CHECK(json::parse(outV1).at("version") == 1);
    CHECK(json::parse(ReadAll(assets / "p.prefab")).at("version") == 1);

    fs::remove_all(dir, ec);
}

int main(int argc, char** argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "--emit-defaults") == 0)
    {
        const std::string text = SceneSerializer::BuildDefaultsTableV2Json();
        std::ofstream ofs(argv[2], std::ios::binary | std::ios::trunc);
        ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
        std::printf("wrote %zu bytes: %s\n", text.size(), argv[2]);
        return ofs ? 0 : 1;
    }

    Test_NormalizeDouble();
    Test_Table();
    Report_TableDrift();
    Test_StripInflateRoundtrip();
    Test_Dump();
    Test_IsV2();
    Test_SceneRoundtrip();
    Test_HandWrittenV2();
    Test_SaveLoadFile();
    Test_RewriteAssetPathRefsInFiles();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
