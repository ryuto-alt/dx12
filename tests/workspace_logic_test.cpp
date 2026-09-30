// ワークスペース / レイアウトの純ロジック（editor/WorkspaceLogic.h）の単体テスト（フェーズ 1b W）。
//   ・分割比のクランプ（NaN / 範囲外）・丸め
//   ・レイアウトの JSON 往復 / 壊れた JSON・未知バージョン・型違い → 既定 / 欠けたキーは既定で補う
//   ・配置先（スロット）の上書き / 既定と同じなら消える / 知らない id の除去
//   ・プリセット 6 種の整合（id 一意・窓 id 重複無し・分割比が範囲内・下部タブの active が tabs に含まれる）
//   ・ワークスペース切替で「最後の状態」が保たれる / 状態の JSON 往復
//   ・名前つきレイアウトの名前検証 / 下部ドック最大化の下限 / フローティング窓の整列
#include "editor/WorkspaceLogic.h"

#include <cstdio>
#include <set>
#include <string>

using namespace dx12e::ws;

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

void TestRatios()
{
    const Ratios d;
    CHECK(Clamp(d) == d, "既定は範囲内でそのまま");
    Ratios r; r.left = 0.9f; r.right = 0.0f; r.bottom = -1.0f; r.rightSplit = 5.0f;
    const Ratios c = Clamp(r);
    CHECK(c.left == kMaxLeft && c.right == kMinRight && c.bottom == kMinBottom && c.rightSplit == kMaxSplit, "範囲外は端へ");
    Ratios n; n.left = std::nanf(""); n.bottom = INFINITY;
    const Ratios cn = Clamp(n);
    CHECK(cn.left == d.left && cn.bottom == d.bottom, "NaN / Inf は既定値");
    CHECK(Round3(0.18049f) == 0.18f && Round3(0.1806f) == 0.181f, "1/1000 丸め");
    CHECK(kMinLeft * 1920.0f >= 190.0f, "1080p の左最小幅");
}

void TestJsonRoundTrip()
{
    Layout l;
    l.ratios.left = 0.21f; l.ratios.right = 0.3f; l.ratios.bottom = 0.4f; l.ratios.rightSplit = 0.5f;
    l.open = {"postProcess", "lighting"};
    SetSlot(l, "skybox", Slot::BottomTab, Slot::RightTab);
    SetSlot(l, "ssao", Slot::RightSplit, Slot::RightTab);
    l.bottomTabs = {"timeline", "x"};
    l.bottomActive = "x";
    Layout o;
    CHECK(FromJson(ToJson(l), o), "往復で成功");
    CHECK(o == l, "往復で一致");
    CHECK(SlotFor(o, "skybox", Slot::RightTab) == Slot::BottomTab, "スロット復元");
    CHECK(SlotFor(o, "none", Slot::Floating) == Slot::Floating, "上書きが無ければ既定");

    Layout e;
    CHECK(FromJson(ToJson(e), o) && o == e, "空レイアウトの往復");
}

void TestBroken()
{
    Layout o;
    for (const char* bad : {"", "{", "null", "[]", "5", "\"x\"", "{}", "{\"v\":99}", "{\"v\":\"1\"}", "{\"v\":1.5}"})
    {
        o.open = {"junk"};
        CHECK(!FromJson(bad, o), "壊れた入力は false: %s", bad);
        CHECK(o == Layout{}, "失敗時 out は既定: %s", bad);
    }
    CHECK(FromJson(R"({"v":1,"ratios":{"left":"x","right":0.3,"bottom":null},"open":[1,"a",null,"a"],"slots":{"a":"nonsense","b":"bottom","c":5},"bottom":{"tabs":"no","active":7}})", o), "部分的に壊れていても読める");
    CHECK(o.ratios.left == Ratios{}.left && o.ratios.right == 0.3f && o.ratios.bottom == Ratios{}.bottom, "壊れた比は既定");
    CHECK(o.open.size() == 1 && o.open[0] == "a", "open は文字列だけ・重複除去");
    CHECK(o.slots.size() == 1 && o.slots[0].id == "b" && o.slots[0].slot == Slot::BottomTab, "不正なスロットは捨てる");
    CHECK(o.bottomTabs.empty() && o.bottomActive.empty(), "型違いの bottom は既定");
    CHECK(FromJson(R"({"v":1,"ratios":{"left":5,"right":-3,"bottom":0.5,"rightSplit":0.5}})", o), "ratios だけ");
    CHECK(o.ratios.left == kMaxLeft && o.ratios.right == kMinRight, "読み込み時にクランプ");
}

void TestSlotsAndSanitize()
{
    Layout l;
    SetSlot(l, "a", Slot::BottomTab, Slot::RightTab);
    CHECK(l.slots.size() == 1, "上書きが入る");
    SetSlot(l, "a", Slot::RightTab, Slot::RightTab);
    CHECK(l.slots.empty(), "既定と同じなら消える");
    SetSlot(l, "a", Slot::Floating, Slot::Floating);
    CHECK(l.slots.empty(), "既定が Floating の窓でも同様");
    SetSlot(l, "a", Slot::RightSplit, Slot::RightTab);
    SetSlot(l, "a", Slot::BottomTab, Slot::RightTab);
    CHECK(l.slots.size() == 1 && l.slots[0].slot == Slot::BottomTab, "同じ id は 1 件");

    l.open = {"a", "gone", "a", "b"};
    SetSlot(l, "gone", Slot::BottomTab, Slot::RightTab);
    l.bottomTabs = {"t", "t"};
    const std::set<std::string> known = {"a", "b"};
    const Layout s = Sanitize(l, [&](const std::string& id) { return known.count(id) != 0; });
    CHECK(s.open.size() == 2 && s.open[0] == "a" && s.open[1] == "b", "未知 id と重複を除く");
    CHECK(s.slots.size() == 1 && s.slots[0].id == "a", "未知 id のスロット上書きも除く");
    CHECK(s.bottomTabs.size() == 1, "下部タブの重複を除く");

    Layout t;
    SetOpen(t, "x", true); SetOpen(t, "x", true); SetOpen(t, "y", true);
    CHECK(t.open.size() == 2, "SetOpen の重複防止");
    SetOpen(t, "x", false);
    CHECK(t.open.size() == 1 && t.open[0] == "y", "SetOpen で閉じる");
}

void TestPresets()
{
    const auto& ps = Presets();
    CHECK(ps.size() == 6, "プリセットは 6 種");
    std::set<std::string> ids;
    for (const auto& p : ps)
    {
        CHECK(ids.insert(p.id).second, "id 一意: %s", p.id);
        CHECK(p.title && p.title[0], "タイトル有り: %s", p.id);
        CHECK(Clamp(p.layout.ratios) == p.layout.ratios, "比が範囲内: %s", p.id);
        CHECK(Dedup(p.layout.open).size() == p.layout.open.size(), "open 重複無し: %s", p.id);
        CHECK(Dedup(p.layout.bottomTabs).size() == p.layout.bottomTabs.size(), "下部タブ重複無し: %s", p.id);
        if (!p.layout.bottomActive.empty())
            CHECK(Contains(p.layout.bottomTabs, p.layout.bottomActive), "active は tabs に含まれる: %s", p.id);
        Layout rt;
        CHECK(FromJson(ToJson(p.layout), rt) && rt == p.layout, "プリセットも JSON 往復: %s", p.id);
    }
    CHECK(FindPreset("level") && FindPreset("material") && FindPreset("lighting") && FindPreset("animation") && FindPreset("vfx") && FindPreset("ui"), "6 種の id");
    CHECK(FindPreset("nope") == nullptr, "未知の id");
    CHECK(FindPreset("level")->layout.open.empty(), "レベル編集 = 全部閉じる（既定）");
    CHECK(FindPreset("animation")->layout.ratios.bottom > Ratios{}.bottom, "アニメ・シーケンサーは下部ドックが厚い");
}

void TestSwitch()
{
    WorkspaceState st;
    Layout cur;
    cur.open = {"lighting"};
    cur.ratios.left = 0.25f;
    Layout out;
    CHECK(SwitchTo(st, "vfx", cur, out), "vfx へ切替");
    CHECK(out == FindPreset("vfx")->layout, "初回はプリセット");
    CHECK(st.current == "vfx", "現在が更新される");
    CHECK(st.saved["level"] == cur, "切替前の状態が level に記憶される");

    Layout vfxNow = out; SetOpen(vfxNow, "particle", false); SetOpen(vfxNow, "postProcess", true);
    CHECK(SwitchTo(st, "level", vfxNow, out), "level へ戻る");
    CHECK(out == cur, "戻ると level の最後の状態（比も開いた窓も）");
    CHECK(SwitchTo(st, "vfx", cur, out) && out == vfxNow, "vfx も最後の状態を保つ");

    const std::string before = st.current;
    CHECK(!SwitchTo(st, "bogus", cur, out) && st.current == before, "未知の id は何も変えない");

    WorkspaceState rt;
    CHECK(StateFromJson(StateToJson(st), rt), "状態の往復");
    CHECK(rt.current == st.current && rt.saved.size() == st.saved.size(), "状態が一致");
    CHECK(rt.saved["level"] == st.saved["level"], "各レイアウトが一致");
    CHECK(!StateFromJson("{oops", rt) && rt.current == "level" && rt.saved.empty(), "壊れた状態は既定");
    CHECK(StateFromJson(R"({"current":"zzz","saved":{"vfx":{"v":1},"nope":{"v":1},"ui":{"v":9}}})", rt), "未知の要素を含んでいても読める");
    CHECK(rt.current == "level" && rt.saved.count("vfx") == 1 && rt.saved.count("nope") == 0 && rt.saved.count("ui") == 0, "未知の id / 壊れたレイアウトは捨てる");
}

void TestNames()
{
    CHECK(NormalizeLayoutName("  作業用  ") == "作業用", "前後の空白を除く");
    CHECK(NormalizeLayoutName("").empty() && NormalizeLayoutName("   ").empty(), "空は不正");
    CHECK(NormalizeLayoutName(std::string(200, 'a')).empty(), "長すぎは不正");
    CHECK(NormalizeLayoutName("a\nb").empty(), "制御文字は不正");
    CHECK(NormalizeLayoutName("ライティング 2") == "ライティング 2", "日本語と内部の空白は可");
}

void TestMaximizeAndCascade()
{
    const float r = MaximizedBottomRatio(800.0f, 200.0f);
    CHECK(std::fabs(r - 0.75f) < 1e-5f, "最大化: ビューポートに 200px 残す");
    CHECK(r > kMaxBottom, "最大化は通常の上限を超えてよい");
    CHECK(MaximizedBottomRatio(100.0f, 200.0f) == kMinBottom, "中央が小さすぎる時は下限");
    CHECK(MaximizedBottomRatio(0.0f, 200.0f) == kMaxBottom, "高さ 0 でも壊れない");
    CHECK(MaximizedBottomRatio(1000.0f, 0.0f) <= 0.92f, "残す高さ 0 でも 0.92 が上限");

    for (int i = 0; i < 20; ++i)
    {
        const Pt p = CascadePos(i, 100, 50, 800, 600, 400, 300, 32);
        CHECK(p.x >= 100 && p.y >= 50 && p.x + 400 <= 900 + 0.01f && p.y + 300 <= 650 + 0.01f, "整列位置は領域内 (%d)", i);
    }
    const Pt small = CascadePos(3, 0, 0, 100, 100, 400, 300, 32);
    CHECK(small.x == 0 && small.y == 0, "領域より窓が大きい時は左上");
    CHECK(CascadePos(1, 0, 0, 800, 600, 100, 100, 30).x > CascadePos(0, 0, 0, 800, 600, 100, 100, 30).x, "順にずれる");
}
} // namespace

int main()
{
    TestRatios();
    TestJsonRoundTrip();
    TestBroken();
    TestSlotsAndSanitize();
    TestPresets();
    TestSwitch();
    TestNames();
    TestMaximizeAndCascade();
    std::printf("WorkspaceLogicTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
