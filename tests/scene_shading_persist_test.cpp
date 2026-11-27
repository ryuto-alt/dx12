// shadingRevision / V2 の欄 / LS-02a の欄のシーン保存と読み込み（W1-FU-E-27）。GPU 不要。
//   ・往復: rev==2 と V2 の欄・Epic の影の欄が、保存 -> 読み込みで一致する
//   ・既定のシーンは "shading" キーを書かない（既存シーンの保存結果が 1 バイトも変わらない）
//   ・範囲外 / 型違いは丸めて読む（rev は 1 / 2 のみ）
//   ・Undo の写し（McpSceneSlot）が before / after を行き来し、設定の指紋が変わる（未保存判定にも載る）
#include "core/mcp/McpUndoTrack.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "scene/SceneSettingsHash.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>

using namespace dx12e;
using json = nlohmann::json;

namespace { int g_failures = 0, g_checks = 0; }
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

namespace
{
std::string Save(const Scene& s) { return SceneSerializer::SaveToStringV2(s, ""); }
json SettingsOf(const std::string& text) { return json::parse(text); }
}

int main()
{
    // ---- 既定のシーンは shading キーを持たない ----
    {
        Scene s;
        const json j = SettingsOf(Save(s));
        CHECK(!j.contains("shading"));
    }

    // ---- 往復（rev==2 + V2 の欄 + LS-02a の欄） ----
    {
        Scene a;
        auto& sh = a.GetShadingSettings();
        sh.shadingRevision = 2; sh.microShadow = false; sh.horizonOcclusion = false; sh.gtso = false;
        sh.microShadowK = 0.5f; sh.horizonK = 1.5f; sh.multiScatter = false;
        auto& pc = a.GetShadowPcssSettings();
        pc.enabled = true; pc.epicDynamicCsm = true; pc.pancaking = false; pc.gatherPcss = false; pc.cascadeDitherBlend = false;
        pc.receiverNormalOffsetBase = 2.5f; pc.receiverNormalOffsetSlope = 0.5f; pc.casterBiasScaleV2 = 0.5f; pc.gatherFilterTaps = 16;

        const std::string text = Save(a);
        Scene b;
        CHECK(SceneSerializer::LoadFromString(b, text, ""));
        const auto& r = b.GetShadingSettings();
        CHECK(r.shadingRevision == 2 && !r.microShadow && !r.horizonOcclusion && !r.gtso && !r.multiScatter);
        CHECK(r.microShadowK == 0.5f && r.horizonK == 1.5f);
        const auto& q = b.GetShadowPcssSettings();
        CHECK(q.enabled && q.epicDynamicCsm && !q.pancaking && !q.gatherPcss && !q.cascadeDitherBlend);
        CHECK(q.receiverNormalOffsetBase == 2.5f && q.receiverNormalOffsetSlope == 0.5f && q.casterBiasScaleV2 == 0.5f && q.gatherFilterTaps == 16u);
        CHECK(Save(b) == text);   // 再保存でバイト一致
    }

    // ---- rev=2 だけ変えたときも保存される / 旧シーン（キー無し）は 1 に戻る ----
    {
        Scene a; a.GetShadingSettings().shadingRevision = 2;
        Scene b; b.GetShadingSettings().shadingRevision = 2;   // 読み込み前に 2 でも、キー無しなら既定へ戻る
        Scene plain;
        CHECK(SceneSerializer::LoadFromString(b, Save(plain), ""));
        CHECK(b.GetShadingSettings().shadingRevision == 1);
        Scene c;
        CHECK(SceneSerializer::LoadFromString(c, Save(a), ""));
        CHECK(c.GetShadingSettings().shadingRevision == 2);
    }

    // ---- 範囲外・不正値は丸める ----
    {
        Scene a; a.GetShadingSettings().shadingRevision = 2;
        json j = SettingsOf(Save(a));
        j["shading"]["shadingRevision"] = 7;
        j["shading"]["microShadowK"] = 99.0;
        j["shadowPcss"]["gatherFilterTaps"] = 1000;
        j["shadowPcss"]["casterBiasScaleV2"] = -3.0;
        Scene b;
        CHECK(SceneSerializer::LoadFromString(b, j.dump(), ""));
        CHECK(b.GetShadingSettings().shadingRevision == 1);
        CHECK(b.GetShadingSettings().microShadowK == 2.0f);
        CHECK(b.GetShadowPcssSettings().gatherFilterTaps == 32u);
        CHECK(b.GetShadowPcssSettings().casterBiasScaleV2 == 0.05f);
    }

    // ---- Undo の写し ----
    {
        Scene s;
        const u64 fp0 = SceneSettingsFingerprint(s);
        McpSceneSlot<ShadingSettings> slot;
        slot.ptr = &s.GetShadingSettings();
        slot.before = *slot.ptr;
        slot.ptr->shadingRevision = 2;
        slot.CaptureAfter();
        CHECK(SceneSettingsFingerprint(s) != fp0);
        slot.Apply(false);
        CHECK(s.GetShadingSettings().shadingRevision == 1 && SceneSettingsFingerprint(s) == fp0);
        slot.Apply(true);
        CHECK(s.GetShadingSettings().shadingRevision == 2);

        McpSceneSlot<ShadowPcssSettings> ps;
        ps.ptr = &s.GetShadowPcssSettings();
        ps.before = *ps.ptr;
        ps.ptr->epicDynamicCsm = true;
        ps.CaptureAfter();
        ps.Apply(false);
        CHECK(!s.GetShadowPcssSettings().epicDynamicCsm);
        ps.Apply(true);
        CHECK(s.GetShadowPcssSettings().epicDynamicCsm);
    }

    // ---- McpUndoTracker（set_shading / set_shadow_pcss がやる Begin -> TrackSceneValue -> 書き換え -> Finish） ----
    {
        Scene s;
        McpUndoTracker t;
        // 値が変わった -> McpSceneStateCommand が 1 つ
        t.Begin(&s, "");
        t.TrackSceneValue(s.GetShadingSettings());
        s.GetShadingSettings().shadingRevision = 2;
        auto cmds = t.Finish();
        CHECK(cmds.size() == 1);
        // 同じ値の再設定（no-op）-> コマンド無し
        t.Begin(&s, "");
        t.TrackSceneValue(s.GetShadingSettings());
        s.GetShadingSettings().shadingRevision = 2;
        CHECK(t.Finish().empty());
        // shadowPcss の LS-02a の欄が変わった -> 1 つ。Undo で戻る
        t.Begin(&s, "");
        t.TrackSceneValue(s.GetShadowPcssSettings());
        s.GetShadowPcssSettings().epicDynamicCsm = true;
        auto c2 = t.Finish();
        CHECK(c2.size() == 1);
        // 検証で弾かれた呼び出し（Track だけして書き換えない）-> コマンド無し
        t.Begin(&s, "");
        t.TrackSceneValue(s.GetShadowPcssSettings());
        CHECK(t.Finish().empty());
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
