// インスペクタの純ロジック（editor/InspectorLogic.h）の単体テスト（フェーズ 1b）。
//   ・Add Component の目録（重複 id なし / カテゴリが順序表にある / 全項目に説明）
//   ・検索ランキング（日本語 / 英語 / 別名 / ラベル優先）・所持済みの除外・最近使ったの先頭固定
//   ・最近使った LRU / 文字列往復 / 折りたたみ状態の往復
//   ・タグ集合（空白 / 全角空白 / 重複 / サジェスト）
//   ・複数選択のバイト比較（Mixed / 既定値との差 / 範囲外）
//   ・Transform の一括編集（触った軸だけ全員へ / 相対）
//   ・プロパティ検索の一致 / 値のコピー・ペースト文字列の往復
// 標準ライブラリだけ（ImGui も entt も GPU も要らない）。
#include "editor/InspectorLogic.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace dx12e::insp;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond);        \
            std::printf(__VA_ARGS__);                                          \
            std::printf("\n");                                                 \
        }                                                                      \
    } while (0)

bool NoneOwned(const char*) { return false; }

void TestCatalog()
{
    std::set<std::string> ids, labels;
    for (int i = 0; i < kCatalogCount; ++i)
    {
        const ComponentInfo& c = kCatalog[i];
        CHECK(c.id && c.id[0], "id が空 #%d", i);
        CHECK(ids.insert(c.id).second, "id 重複 %s", c.id);
        CHECK(labels.insert(c.label).second, "label 重複 %s", c.label);
        CHECK(CategoryRank(c.category) < kCategoryCount, "カテゴリが順序表に無い %s / %s", c.id, c.category);
        CHECK(c.desc && c.desc[0], "説明が空 %s", c.id);
        CHECK(c.keywords && c.keywords[0], "別名が空 %s", c.id);
    }
    CHECK(FindCatalog("PointLight") >= 0, "PointLight");
    CHECK(FindCatalog("Nope") == -1, "未知 id");
    // 既存 UI テスト（add_all_components）が部分文字列で引く英語ラベルがすべて残っている
    const char* must[] = {"Point Light", "Directional Light", "Spot Light", "Camera", "Audio Source", "Gimmick",
                          "Particle Emitter", "Trail Renderer", "Trigger", "UI Canvas", "UI Rect", "UI Image",
                          "UI Text", "UI Button", "UI Slider", "UI Toggle", "UI Scroll View", "UI Layout",
                          "UI Animator", "RigidBody", "Box Collider", "Sphere Collider", "Capsule Collider",
                          "Character Controller", "Network Identity", "Network Transform"};
    for (const char* m : must)
    {
        bool found = false;
        for (int i = 0; i < kCatalogCount; ++i)
            if (std::string(kCatalog[i].label).find(m) != std::string::npos) { found = true; break; }
        CHECK(found, "ラベルに %s が無い", m);
    }
}

void TestRank()
{
    int recentN = -1;
    // 空クエリ・最近使った無し: 全件がカテゴリ順（先頭はライト）
    {
        auto r = RankCatalog("", {}, NoneOwned, &recentN);
        CHECK(static_cast<int>(r.size()) == kCatalogCount, "空クエリは全件 %zu", r.size());
        CHECK(recentN == 0, "最近使った枠は 0");
        CHECK(std::string(kCatalog[r.front()].category) == "ライト", "先頭はライト");
        int prev = -1; bool ok = true;
        for (int i : r) { const int cr = CategoryRank(kCatalog[i].category); if (cr < prev) ok = false; prev = cr; }
        CHECK(ok, "カテゴリ順に並ぶ");
    }
    // 最近使った: 先頭に来て、二重には出ない
    {
        RecentList rec = {"Brain", "PointLight", "Nope"};
        auto r = RankCatalog("", rec, NoneOwned, &recentN);
        CHECK(recentN == 2, "未知 id は捨てる: %d", recentN);
        CHECK(std::string(kCatalog[r[0]].id) == "Brain" && std::string(kCatalog[r[1]].id) == "PointLight", "最近使った順");
        int cnt = 0; for (int i : r) if (std::string(kCatalog[i].id) == "Brain") ++cnt;
        CHECK(cnt == 1, "二重に出ない");
        CHECK(static_cast<int>(r.size()) == kCatalogCount, "全件");
    }
    // 所持済みは出ない（最近使ったの中でも）
    {
        auto owned = [](const char* id) { return std::string(id) == "Brain" || std::string(id) == "PointLight"; };
        auto r = RankCatalog("", {"Brain", "SpotLight"}, owned, &recentN);
        CHECK(recentN == 1, "所持済みは最近使ったから外れる");
        CHECK(std::string(kCatalog[r[0]].id) == "SpotLight", "残りが先頭");
        for (int i : r) CHECK(!owned(kCatalog[i].id), "所持済みが出ている %s", kCatalog[i].id);
        auto q = RankCatalog("light", {}, owned);
        for (int i : q) CHECK(!owned(kCatalog[i].id), "検索でも所持済みは出ない");
    }
    // 英語ラベル検索
    {
        auto r = RankCatalog("point", {}, NoneOwned);
        CHECK(!r.empty() && std::string(kCatalog[r[0]].id) == "PointLight", "point → Point Light が先頭");
        auto r2 = RankCatalog("rigid", {}, NoneOwned);
        CHECK(!r2.empty() && std::string(kCatalog[r2[0]].id) == "RigidBody", "rigid → RigidBody が先頭");
    }
    // 日本語（別名・説明・カテゴリ）
    {
        auto r = RankCatalog("剛体", {}, NoneOwned);
        bool hasRb = false; for (int i : r) if (std::string(kCatalog[i].id) == "RigidBody") hasRb = true;
        CHECK(hasRb, "剛体 → RigidBody（別名）");
        auto r2 = RankCatalog("当たり判定", {}, NoneOwned);
        int colliders = 0;
        for (int i : r2) if (std::string(kCatalog[i].id).find("Collider") != std::string::npos) ++colliders;
        CHECK(colliders == 3, "当たり判定 → コライダー 3 種 (%d)", colliders);
        auto r3 = RankCatalog("ライト", {}, NoneOwned);
        CHECK(r3.size() >= 3, "ライト → 3 件以上");
        CHECK(std::string(kCatalog[r3[0]].category) == "ライト", "ライトのカテゴリが先頭");
        // カタカナ / ひらがなの同一視（FuzzyMatch の仕様）
        auto r4 = RankCatalog("ぱーてぃくる", {}, NoneOwned);
        CHECK(!r4.empty() && std::string(kCatalog[r4[0]].id) == "ParticleEmitter", "ぱーてぃくる → Particle");
    }
    // ラベル一致は説明だけの一致より上
    {
        auto r = RankCatalog("ui", {}, NoneOwned);
        CHECK(!r.empty() && std::string(kCatalog[r[0]].category) == "UI", "ui → UI 系が先頭");
    }
    // 不一致
    {
        auto r = RankCatalog("zzzzqqqq", {}, NoneOwned);
        CHECK(r.empty(), "不一致は空");
    }
}

void TestRecentAndFold()
{
    RecentList r;
    PushRecent(r, "A"); PushRecent(r, "B"); PushRecent(r, "C");
    CHECK(r.size() == 3 && r[0] == "C" && r[2] == "A", "新しい順");
    PushRecent(r, "A");
    CHECK(r.size() == 3 && r[0] == "A" && r[1] == "C" && r[2] == "B", "再利用で先頭へ・重複しない");
    for (int i = 0; i < 20; ++i) PushRecent(r, "X" + std::to_string(i), 5);
    CHECK(r.size() == 5 && r[0] == "X19", "上限 5");
    PushRecent(r, "");
    CHECK(r.size() == 5, "空 id は無視");

    const std::string s = JoinList(r);
    CHECK(SplitList(s) == r, "往復");
    CHECK(SplitList("a,,b,").size() == 2, "空要素は捨てる");
    CHECK(SplitList("").empty(), "空文字列");

    FoldList f;
    SetFold(f, "Transform", true); SetFold(f, "MeshRenderer", false); SetFold(f, "Transform", false);
    CHECK(f.size() == 2, "上書きで増えない");
    CHECK(GetFold(f, "Transform") == 0 && GetFold(f, "MeshRenderer") == 0 && GetFold(f, "Camera") == -1, "取得");
    SetFold(f, "Camera", true);
    const FoldList g = DecodeFold(EncodeFold(f));
    CHECK(g.size() == 3 && GetFold(g, "Camera") == 1 && GetFold(g, "Transform") == 0, "折りたたみの往復");
    CHECK(DecodeFold("broken;=1;x=").empty(), "壊れた要素は捨てる");
    CHECK(DecodeFold("Box Collider=1").size() == 1 && GetFold(DecodeFold("Box Collider=1"), "Box Collider") == 1, "空白入りの名前");
}

void TestTags()
{
    std::vector<std::string> t;
    CHECK(AddTag(t, "enemy") && t.size() == 1, "追加");
    CHECK(!AddTag(t, "enemy"), "重複は不可");
    CHECK(!AddTag(t, "   "), "空白だけは不可");
    CHECK(AddTag(t, "  boss \t") && t.back() == "boss", "前後の空白を落とす");
    CHECK(AddTag(t, "\xE3\x80\x80" "敵" "\xE3\x80\x80") && t.back() == "\xE6\x95\xB5", "全角空白も落とす");
    CHECK(AddTag(t, "Enemy") && t.size() == 4, "大小は別物");
    CHECK(RemoveTag(t, "boss") && t.size() == 3, "削除");
    CHECK(!RemoveTag(t, "boss"), "無いものは false");

    std::vector<std::string> all = {"enemy", "enemy", "boss", "player", "enemy", "pickup", "pickup"};
    auto s = SuggestTags(all, {"boss"}, "");
    CHECK(!s.empty() && s[0] == "enemy", "出現数の多い順");
    for (auto& x : s) CHECK(x != "boss", "既に付いているものは出ない");
    auto q = SuggestTags(all, {}, "pl");
    CHECK(!q.empty() && q[0] == "player", "pl → player");
    CHECK(SuggestTags(all, {}, "zzz").empty(), "不一致");
    CHECK(SuggestTags(all, {}, "", 2).size() == 2, "上限");
}

struct Pod { float a[3]; int n; bool b; float c; };

void TestBytes()
{
    Pod p{{1, 2, 3}, 5, true, 0.5f};
    Pod o1 = p, o2 = p;
    const unsigned char* pp = reinterpret_cast<const unsigned char*>(&p);
    const unsigned char* others[2] = {reinterpret_cast<const unsigned char*>(&o1), reinterpret_cast<const unsigned char*>(&o2)};
    CHECK(!BytesMixed(pp, others, 2, sizeof(float)), "同じなら Mixed でない");
    o2.a[1] = 9.0f;
    const size_t offY = offsetof(Pod, a) + sizeof(float);
    CHECK(BytesMixed(pp + offY, [&] { static const unsigned char* q[2]; q[0] = reinterpret_cast<const unsigned char*>(&o1) + offY; q[1] = reinterpret_cast<const unsigned char*>(&o2) + offY; return q; }(), 2, sizeof(float)), "Y だけ Mixed");
    const unsigned char* oX[2] = {reinterpret_cast<const unsigned char*>(&o1), reinterpret_cast<const unsigned char*>(&o2)};
    CHECK(!BytesMixed(pp, oX, 2, sizeof(float)), "X は揃っている");
    CHECK(!BytesMixed(pp, nullptr, 0, 4), "他が無ければ Mixed でない");

    Pod d{{0, 0, 0}, 5, false, 0.5f};
    const unsigned char* dp = reinterpret_cast<const unsigned char*>(&d);
    CHECK(BytesDiffer(pp + offsetof(Pod, a), dp + offsetof(Pod, a), sizeof(float) * 3), "a は既定と違う");
    CHECK(!BytesDiffer(pp + offsetof(Pod, n), dp + offsetof(Pod, n), sizeof(int)), "n は既定と同じ");
    CHECK(BytesDiffer(pp + offsetof(Pod, b), dp + offsetof(Pod, b), sizeof(bool)), "b は違う");
    CHECK(!BytesDiffer(pp, nullptr, 4), "既定が無ければ印を出さない");

    CHECK(OffsetIn(&p, sizeof(p), &p.c, sizeof(float)) == static_cast<long>(offsetof(Pod, c)), "範囲内の offset");
    float outside = 0;
    const long off = OffsetIn(&p, sizeof(p), &outside, sizeof(float));
    CHECK(off == -1 || (off >= 0 && static_cast<size_t>(off) + 4 <= sizeof(p)), "範囲外/内の判定が矛盾しない");
    CHECK(OffsetIn(&p, sizeof(p), reinterpret_cast<const char*>(&p) + sizeof(p) - 2, 4) == -1, "はみ出しは -1");
    CHECK(OffsetIn(nullptr, 0, &p, 4) == -1, "null は -1");
}

void TestTransformEdit()
{
    const Vec3 before{1, 2, 3};
    Vec3 edited = before; edited.y = 10;   // Y だけ触った
    Vec3 other{7, 8, 9};
    CHECK(ApplyTouchedAxes(other, before, edited), "書き換えた");
    CHECK(other.x == 7 && other.y == 10 && other.z == 9, "触った軸 Y だけ全員へ（X / Z は各自のまま）");

    Vec3 same{7, 10, 9};
    CHECK(!ApplyTouchedAxes(same, before, edited), "既に同じなら書き換え無し");
    Vec3 untouched = before;
    Vec3 o2{4, 5, 6};
    CHECK(!ApplyTouchedAxes(o2, before, untouched) && o2.x == 4, "何も触っていなければ無操作");

    Vec3 e3{5, 2, 7};   // X と Z を触った
    Vec3 o3{0, 0, 0};
    CHECK(ApplyTouchedAxes(o3, before, e3) && o3.x == 5 && o3.y == 0 && o3.z == 7, "複数軸");

    Vec3 rel{10, 20, 30};
    Vec3 e4{1.5f, 2, 3};
    CHECK(ApplyDeltaAxes(rel, before, e4) && std::fabs(rel.x - 10.5f) < 1e-6f && rel.y == 20 && rel.z == 30, "相対は差分を足す");
    Vec3 rel2{1, 1, 1};
    CHECK(!ApplyDeltaAxes(rel2, before, before), "差分ゼロは無操作");
}

void TestMatchAndClipboard()
{
    CHECK(PropertyMatches("", "位置 Position"), "空フィルタは全一致");
    CHECK(PropertyMatches("pos", "位置 Position"), "英語");
    CHECK(PropertyMatches("位置", "位置 Position"), "日本語");
    CHECK(!PropertyMatches("scale", "位置 Position"), "不一致");
    CHECK(PropertyMatches("ROT", "回転 Rotation"), "大小無視");

    float v[3] = {1.5f, -2.0f, 3.25f};
    const std::string enc = EncodeFloats(v, 3);
    double out[3] = {};
    CHECK(DecodeNumbers(enc, 3, out) && out[0] == 1.5 && out[1] == -2.0 && out[2] == 3.25, "float3 の往復");
    CHECK(!DecodeNumbers(enc, 2, out), "個数が合わなければ不可");
    CHECK(!DecodeNumbers(enc, 4, out), "足りなければ不可");
    double one[1] = {};
    CHECK(DecodeNumbers(EncodeInt(-7), 1, one) && one[0] == -7, "int");
    CHECK(DecodeNumbers(EncodeBool(true), 1, one) && one[0] == 1, "bool true");
    CHECK(DecodeNumbers(EncodeBool(false), 1, one) && one[0] == 0, "bool false");
    CHECK(!DecodeNumbers("hello", 1, one), "型タグの無い文字列は不可");
    CHECK(!DecodeNumbers("dx12v:f:1,abc", 2, out), "壊れた数値は不可");
    CHECK(!DecodeNumbers("dx12v:f:1,", 2, out), "末尾の空要素は不可");
    // float の精度が往復で落ちない
    float pi = 3.14159274f;
    double po[1] = {};
    CHECK(DecodeNumbers(EncodeFloats(&pi, 1), 1, po) && static_cast<float>(po[0]) == pi, "9 桁で往復する");
}
} // namespace

int main()
{
    TestCatalog();
    TestRank();
    TestRecentAndFold();
    TestTags();
    TestBytes();
    TestTransformEdit();
    TestMatchAndClipboard();
    std::printf("InspectorLogicTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
