// GI モード「新」の既定と移行（GI_FOUNDATION_DESIGN §2 S5）の単体テスト。GPU も窓も要らない。
//   ・DDGI 格子の自動フィット（純関数）: 1.0m 間隔 / 4096 個以下 / 軸 32 以下 / 範囲を覆う / 平らなシーンは高さを足す / 巨大な背景は無視
//   ・新の既定構成（gi・DDGI・多重バウンス・SSGI・RT 影・太陽の ambient 0）/ DXR 非対応では何も変えない / 旧へ戻す
//   ・移行の Undo / Redo が 1 操作で全項目を戻す（GiMigrationCommand）
//   ・保存互換: 既定 / 旧のシーンは gi キーを書かない / 新は書いて往復する / キーの無い JSON は旧として読む
//   ・テンプレート: 3D（empty / fps / tps）の全シーンが gi=new + 焼いた DDGI 格子 + ambient 0、2d は旧のまま

#include "editor/GiMigrationCommand.h"
#include "project/ProjectTemplates.h"
#include "scene/GiMigration.h"
#include "scene/Scene.h"
#include "scene/SceneSerializer.h"
#include "renderer/Mesh.h"   // Scene のデストラクタが Mesh の完全型を要る

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace dx12e;
using json = nlohmann::json;

namespace
{
int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

gi::Box MakeBox(float x0, float y0, float z0, float x1, float y1, float z1, bool dyn = false)
{
    gi::Box b;
    b.mn = {x0, y0, z0};
    b.mx = {x1, y1, z1};
    b.isDynamic = dyn;
    return b;
}

int Total(const DdgiSettings& d) { return d.probeCountX * d.probeCountY * d.probeCountZ; }

// 格子の全プローブ（最小隅〜最大隅）が範囲の外側へ張り出している（= 範囲を覆う）
bool Covers(const DdgiSettings& d, const gi::Bounds& b)
{
    const float lastX = d.originX + (d.probeCountX - 1) * d.spacing;
    const float lastY = d.originY + (d.probeCountY - 1) * d.spacing;
    const float lastZ = d.originZ + (d.probeCountZ - 1) * d.spacing;
    constexpr float e = 1.0e-3f;
    return d.originX <= b.mn.x + e && d.originY <= b.mn.y + e && d.originZ <= b.mn.z + e
        && lastX >= b.mx.x - e && lastY >= b.mx.y - e && lastZ >= b.mx.z - e;
}

gi::Bounds MakeBounds(float x0, float y0, float z0, float x1, float y1, float z1)
{
    gi::Bounds b;
    b.mn = {x0, y0, z0};
    b.mx = {x1, y1, z1};
    b.valid = true;
    return b;
}

// ---------------------------------------------------------------------------
void TestFit()
{
    // 部屋 6 x 3 x 6 m: 間隔 1.0 のまま・範囲を覆う・上限内
    {
        const gi::Bounds b = MakeBounds(0, 0, 0, 6, 3, 6);
        DdgiSettings base;
        base.intensity = 0.7f;
        base.hysteresis = 0.9f;
        const DdgiSettings d = gi::FitDdgiToBounds(base, b);
        CHECK(d.enabled);
        CHECK(d.spacing == 1.0f);
        CHECK(Total(d) <= 4096 && d.probeCountX <= 32 && d.probeCountY <= 32 && d.probeCountZ <= 32);
        CHECK(Covers(d, MakeBounds(0, 0, 0, 6, 4, 6)));   // 高さは 4m まで確保される
        CHECK(d.intensity == 0.7f && d.hysteresis == 0.9f);   // 格子以外の項目は引き継ぐ
        // 格子は範囲の中心に揃う（左右の張り出しが等しい）
        const float lastX = d.originX + (d.probeCountX - 1) * d.spacing;
        CHECK(std::fabs((b.mn.x - d.originX) - (lastX - b.mx.x)) < 1.0e-3f);
    }
    // 平らなシーン（高さ 0）でも縦に kFitMinHeight 以上
    {
        const gi::Bounds b = MakeBounds(-10, 0, -10, 10, 0, 10);
        const DdgiSettings d = gi::FitDdgiToBounds(DdgiSettings{}, b);
        CHECK(d.originY + (d.probeCountY - 1) * d.spacing >= gi::kFitMinHeight - 1.0e-3f);
        CHECK(d.spacing == 1.0f && Total(d) <= 4096);
    }
    // 広いシーン: 間隔が広がって 4096 / 32 に収まる
    {
        const gi::Bounds b = MakeBounds(-100, 0, -100, 100, 12, 100);
        const DdgiSettings d = gi::FitDdgiToBounds(DdgiSettings{}, b);
        CHECK(d.spacing > 1.0f);
        CHECK(Total(d) <= 4096 && d.probeCountX <= 32 && d.probeCountZ <= 32);
        CHECK(Covers(d, b));
        // 0.25m 刻み
        CHECK(std::fabs(d.spacing * 4.0f - std::round(d.spacing * 4.0f)) < 1.0e-4f);
    }
    // 不正な範囲（箱が無い）: 既定の部屋 1 つぶん
    {
        const DdgiSettings d = gi::FitDdgiToBounds(DdgiSettings{}, gi::Bounds{});
        CHECK(d.enabled && Total(d) <= 4096 && d.spacing == 1.0f);
        CHECK(Covers(d, MakeBounds(-8, 0, -8, 8, 4, 8)));
    }
    // 性質テスト: いろいろな大きさで上限と被覆が必ず成り立つ
    {
        const float sizes[] = {0.5f, 3.0f, 12.0f, 40.0f, 90.0f, 250.0f, 900.0f};
        for (float sx : sizes)
            for (float sy : {0.0f, 2.5f, 30.0f})
                for (float sz : sizes)
                {
                    const gi::Bounds b = MakeBounds(5, -2, -7, 5 + sx, -2 + sy, -7 + sz);
                    const DdgiSettings d = gi::FitDdgiToBounds(DdgiSettings{}, b);
                    CHECK(Total(d) <= 4096);
                    CHECK(d.probeCountX >= 2 && d.probeCountY >= 2 && d.probeCountZ >= 2);
                    CHECK(d.probeCountX <= 32 && d.probeCountY <= 32 && d.probeCountZ <= 32);
                    CHECK(d.spacing >= 1.0f && d.spacing <= 100.0f);
                    // 平坦 / 巨大でも、高さ方向込みで被覆する
                    gi::Bounds bb = b;
                    if (bb.mx.y - bb.mn.y < gi::kFitMinHeight) bb.mx.y = bb.mn.y + gi::kFitMinHeight;
                    CHECK(Covers(d, bb));
                }
    }
}

void TestChooseBounds()
{
    // 動く物（Dynamic な剛体）は数えない
    {
        std::vector<gi::Box> v = {MakeBox(0, 0, 0, 10, 3, 10), MakeBox(40, 0, 40, 41, 1, 41, true)};
        const gi::Bounds b = gi::ChooseBounds(v);
        CHECK(b.valid && b.mx.x == 10.0f && b.mx.z == 10.0f);
    }
    // 巨大な背景（水平 100m 超）は、他の箱があれば無視する
    {
        std::vector<gi::Box> v = {MakeBox(-5000, -0.1f, -5000, 5000, 0, 5000), MakeBox(0, 0, 0, 8, 3, 8)};
        const gi::Bounds b = gi::ChooseBounds(v);
        CHECK(b.valid && b.mn.x == 0.0f && b.mx.x == 8.0f);
    }
    // 巨大な箱しか無いときは中心のまわり 100m に切り取る
    {
        std::vector<gi::Box> v = {MakeBox(-5000, 0, -5000, 5000, 0, 5000)};
        const gi::Bounds b = gi::ChooseBounds(v);
        CHECK(b.valid && std::fabs((b.mx.x - b.mn.x) - gi::kFitHugeClamp) < 1.0e-3f);
    }
    // 動かない物が無ければ動く物を使う / 空なら無効
    {
        std::vector<gi::Box> v = {MakeBox(1, 1, 1, 2, 2, 2, true)};
        CHECK(gi::ChooseBounds(v).valid);
        CHECK(!gi::ChooseBounds({}).valid);
    }
}

// ---------------------------------------------------------------------------
void AddSun(Scene& s, float ambient)
{
    auto& reg = s.GetRegistry();
    const entt::entity e = reg.create();
    reg.emplace<NameTag>(e, NameTag{"Sun"});
    reg.emplace<Transform>(e, Transform{});
    DirectionalLight dl;
    dl.ambient = ambient;
    reg.emplace<DirectionalLight>(e, dl);
}

void TestApply()
{
    // 新規シーン相当: 既定は旧・DDGI/SSGI/RT 影 OFF・ambient 0.25
    Scene s;
    AddSun(s, 0.25f);
    AddSun(s, 0.4f);
    CHECK(s.GetGiSettings().mode == GiMode::Legacy);
    CHECK(!s.GetDdgiSettings().enabled && !s.GetSsgiSettings().enabled && !s.GetRtSettings().shadowEnabled);

    // DXR 非対応: 何も変えない・理由が返る
    {
        gi::Options o;
        o.dxrSupported = false;
        const gi::State before = gi::Capture(s);
        const gi::Result r = gi::ApplyNew(s, o);
        CHECK(!r.applied && !r.reason.empty());
        CHECK(s.GetGiSettings().mode == GiMode::Legacy);
        CHECK(!s.GetDdgiSettings().enabled && !s.GetSsgiSettings().enabled);
        for (const auto& [e, amb] : before.ambient) CHECK(s.GetRegistry().get<DirectionalLight>(e).ambient == amb);
        o.whyNot = "テスト用の理由";
        CHECK(gi::ApplyNew(s, o).reason == "テスト用の理由");
    }

    // 新: 構成の全項目
    const gi::State legacyState = gi::Capture(s);
    gi::Options o;
    const gi::Result r = gi::ApplyNew(s, o);
    CHECK(r.applied && r.fitted && !r.fromGeometry);   // メッシュが無いので既定の範囲
    CHECK(s.GetGiSettings().mode == GiMode::New);
    CHECK(s.GetDdgiSettings().enabled && s.GetDdgiSettings().bounceIntensity == 1.0f);
    CHECK(Total(s.GetDdgiSettings()) <= 4096);
    // GI S4: 新しい既定は「カメラ追従のスクロール格子 + 2 カスケード」（シーンの AABB に依らない）
    {
        const DdgiSettings& d = s.GetDdgiSettings();
        CHECK(d.followCamera);
        CHECK(d.probeCountX == gi::kFollowCountX && d.probeCountY == gi::kFollowCountY && d.probeCountZ == gi::kFollowCountZ);
        CHECK(d.spacing == gi::kFollowSpacing && d.spacing1 == gi::kFollowSpacing1 && d.spacing1 > d.spacing);
        CHECK(Total(d) <= static_cast<int>(DdgiVolume::kMaxProbes));   // 1 カスケードぶんが上限内（全体は 2 倍）
    }
    CHECK(s.GetSsgiSettings().enabled && s.GetRtSettings().shadowEnabled);
    CHECK(r.ambientChanged == 2);
    for (auto [e, dl] : s.GetRegistry().view<DirectionalLight>().each()) { (void)e; CHECK(dl.ambient == 0.0f); }

    // Undo / Redo は 1 操作で全項目を戻す
    const gi::State newState = gi::Capture(s);
    GiMigrationCommand cmd(&s, legacyState, newState, "GI を新しい方式へ切り替え");
    cmd.Undo();
    CHECK(s.GetGiSettings().mode == GiMode::Legacy);
    CHECK(!s.GetDdgiSettings().enabled && !s.GetSsgiSettings().enabled && !s.GetRtSettings().shadowEnabled);
    {
        std::vector<float> amb;
        for (auto [e, dl] : s.GetRegistry().view<DirectionalLight>().each()) { (void)e; amb.push_back(dl.ambient); }
        std::sort(amb.begin(), amb.end());
        CHECK(amb.size() == 2 && amb[0] == 0.25f && amb[1] == 0.4f);
    }
    cmd.Redo();
    CHECK(s.GetGiSettings().mode == GiMode::New && s.GetDdgiSettings().enabled && s.GetSsgiSettings().enabled);

    // 手置きの格子（DDGI が既に ON）は refitGrid=false なら残る / true なら置き直す
    {
        s.GetDdgiSettings().probeCountX = 5; s.GetDdgiSettings().spacing = 3.0f;
        gi::Options keep; keep.refitGrid = false;
        const gi::Result k = gi::ApplyNew(s, keep);
        CHECK(k.applied && !k.fitted && s.GetDdgiSettings().probeCountX == 5 && s.GetDdgiSettings().spacing == 3.0f);
        gi::Options refit; refit.refitGrid = true;
        const gi::Result f = gi::ApplyNew(s, refit);
        CHECK(f.fitted && s.GetDdgiSettings().followCamera && s.GetDdgiSettings().spacing == gi::kFollowSpacing);
        // シーンへ合わせた固定ボリュームを選ぶ（従来の自動フィット）
        gi::Options fit; fit.refitGrid = true; fit.fitToScene = true;
        const gi::Result fs = gi::ApplyNew(s, fit);
        CHECK(fs.fitted && !s.GetDdgiSettings().followCamera && s.GetDdgiSettings().spacing == 1.0f);
        // DDGI が OFF なら refitGrid=false でも置く
        s.GetDdgiSettings().enabled = false;
        const gi::Result z = gi::ApplyNew(s, keep);
        CHECK(z.fitted && s.GetDdgiSettings().enabled);
    }
    // 格子だけ置き直す（モード・ambient・SSGI は触らない）
    {
        s.GetSsgiSettings().enabled = false;
        const gi::Result g = gi::RefitGrid(s);
        CHECK(g.applied && g.fitted && !s.GetSsgiSettings().enabled && s.GetGiSettings().mode == GiMode::New);
        for (auto [e, dl] : s.GetRegistry().view<DirectionalLight>().each()) { (void)e; CHECK(dl.ambient == 0.0f); }
    }

    // 旧へ戻す: mode / DDGI OFF / ambient 0 の太陽は 0.25。SSGI・RT 影は触らない
    {
        s.GetSsgiSettings().enabled = true;
        const gi::Result l = gi::ApplyLegacy(s);
        CHECK(l.applied && l.ambientChanged == 2);
        CHECK(s.GetGiSettings().mode == GiMode::Legacy && !s.GetDdgiSettings().enabled);
        CHECK(s.GetSsgiSettings().enabled && s.GetRtSettings().shadowEnabled);
        for (auto [e, dl] : s.GetRegistry().view<DirectionalLight>().each()) { (void)e; CHECK(dl.ambient == gi::kUndoAmbientBack); }
    }
}

// ---------------------------------------------------------------------------
std::string ReadAll(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void TestSerialization()
{
    const std::string path = "gi_migration_test_scene.json";
    // 既定（旧）のシーンは gi キーを書かない
    {
        Scene s;
        CHECK(SceneSerializer::Save(s, path, ""));
        const json j = json::parse(ReadAll(path));
        CHECK(!j.contains("gi"));
    }
    // キーの無い JSON は旧として読み、保存し直しても gi キーは増えない（勝手に移行しない）
    {
        Scene s;
        CHECK(SceneSerializer::Load(s, path, ""));
        CHECK(s.GetGiSettings().mode == GiMode::Legacy);
        CHECK(SceneSerializer::Save(s, path, ""));
        CHECK(!json::parse(ReadAll(path)).contains("gi"));
    }
    // 新へ移行して保存 → gi.mode と DDGI/SSGI/RT 影が書かれ、読み戻しで同じ
    {
        Scene s;
        AddSun(s, 0.25f);
        gi::ApplyNew(s, gi::Options{});
        CHECK(SceneSerializer::Save(s, path, ""));
        const json j = json::parse(ReadAll(path));
        CHECK(j.contains("gi") && j["gi"].value("mode", "") == "new");
        CHECK(j["raytracing"]["ddgi"].value("enabled", false));
        CHECK(j["raytracing"].value("shadowEnabled", false));
        CHECK(j["ssgi"].value("enabled", false));
        Scene d;
        CHECK(SceneSerializer::Load(d, path, ""));
        CHECK(d.GetGiSettings().mode == GiMode::New);
        CHECK(d.GetDdgiSettings().enabled && d.GetDdgiSettings().probeCountX == s.GetDdgiSettings().probeCountX);
        // GI S4: カメラ追従 / 遠景の間隔が往復する。既定のままの項目は書かない（旧シーンの JSON は変わらない）
        CHECK(j["raytracing"]["ddgi"].value("followCamera", false));
        CHECK(d.GetDdgiSettings().followCamera && d.GetDdgiSettings().spacing1 == s.GetDdgiSettings().spacing1);
        CHECK(!j["raytracing"]["ddgi"].contains("budgetMs"));
        CHECK(d.GetSsgiSettings().enabled && d.GetRtSettings().shadowEnabled);
        for (auto [e, dl] : d.GetRegistry().view<DirectionalLight>().each()) { (void)e; CHECK(dl.ambient == 0.0f); }
        // 旧へ戻して保存 → gi キーが消える
        gi::ApplyLegacy(d);
        CHECK(SceneSerializer::Save(d, path, ""));
        CHECK(!json::parse(ReadAll(path)).contains("gi"));
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
void TestTemplates()
{
    int scenes3d = 0;
    for (const char* id : {"empty", "fps", "tps", "2d"})
    {
        const bool is3d = std::string_view(id) != "2d";
        for (const auto& f : templates::GetFiles(id))
        {
            const std::string_view rel = f.relPath;
            if (rel.rfind("assets/scenes/", 0) != 0) continue;
            const json j = json::parse(f.content);
            if (!is3d)
            {
                // 2D（正射カメラのスプライト主体）は旧のまま: gi キーを持たない
                CHECK(!j.contains("gi"));
                continue;
            }
            ++scenes3d;
            CHECK(j.contains("gi") && j["gi"].value("mode", "") == "new");
            CHECK(j.contains("ssgi") && j["ssgi"].value("enabled", false));
            CHECK(j.contains("raytracing") && j["raytracing"].value("shadowEnabled", false));
            const json dd = j.value("raytracing", json::object()).value("ddgi", json::object());
            CHECK(dd.value("enabled", false));
            const int nx = dd.value("probeCountX", 0), ny = dd.value("probeCountY", 0), nz = dd.value("probeCountZ", 0);
            CHECK(nx >= 2 && ny >= 2 && nz >= 2 && nx <= 32 && ny <= 32 && nz <= 32);
            CHECK(nx * ny * nz <= static_cast<int>(DdgiVolume::kMaxProbes));
            CHECK(dd.value("followCamera", false));   // GI S4: 新規シーンの既定はカメラ追従（1 カスケードの格子数・近景の間隔）
            CHECK(dd.value("spacing", 0.0f) >= 0.5f && dd.value("spacing1", 0.0f) > dd.value("spacing", 0.0f));
            CHECK(dd.value("bounceIntensity", 0.0f) == 1.0f);
            for (const auto& e : j["entities"])
                if (e.contains("directionalLight"))
                    CHECK(e["directionalLight"].value("ambient", 1.0f) == 0.0f);
        }
    }
    CHECK(scenes3d == 7);   // empty 1 + fps 3 + tps 3
}

} // namespace

int main()
{
    TestFit();
    TestChooseBounds();
    TestApply();
    TestSerialization();
    TestTemplates();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
