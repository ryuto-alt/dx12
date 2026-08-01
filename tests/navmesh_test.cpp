// 自作ナビメッシュの単体テスト。nav/ は純ロジック（GPU も entt も DirectXMath も不要）なので
// テスト側で .cpp を直接ビルドしている。実行: ctest --output-on-failure -R NavMeshTests
//
// ここで守りたい不変条件:
//   ① 平床が歩ける面になり、まっすぐ渡れる（ファネルが余計な折れを作らない）
//   ② 傾斜が agentMaxSlope で正しく切られる（30度は歩ける / 70度は歩けない）★坂道
//   ③ 段差は agentMaxClimb で繋がる / 繋がらない
//   ④ エージェント半径ぶん壁から削られる（半径より狭い通路は通れない）
//   ⑤ 障害物を避ける経路が「回り込む」形になり、直線より長い
//   ⑥ 坂の上の高さ取得がボクセル分解能で追随する
//   ⑦ レイキャストが壁で止まる / 開けた方向では止まらない
//   ⑧ 保存→読み込みで内容が一致する

#include "nav/NavTypes.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dx12e;
using namespace dx12e::nav;

namespace { int g_failures = 0, g_checks = 0; }

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace
{

void AddQuad(NavInputGeometry& g,
             float ax, float ay, float az, float bx, float by, float bz,
             float cx, float cy, float cz, float dx, float dy, float dz)
{
    const int base = static_cast<int>(g.verts.size() / 3);
    g.verts.insert(g.verts.end(), { ax, ay, az, bx, by, bz, cx, cy, cz, dx, dy, dz });
    g.tris.insert(g.tris.end(), { base + 0, base + 1, base + 2, base + 0, base + 2, base + 3 });
}

// y 一定の床（xz 範囲）
void AddFloor(NavInputGeometry& g, float x0, float z0, float x1, float z1, float y)
{
    AddQuad(g, x0, y, z0, x1, y, z0, x1, y, z1, x0, y, z1);
}

// x 一定の壁（z 範囲 × y 範囲）
void AddWallX(NavInputGeometry& g, float x, float z0, float z1, float y0, float y1)
{
    AddQuad(g, x, y0, z0, x, y0, z1, x, y1, z1, x, y1, z0);
}

// z 方向へ上る斜面（y0 → y1）
void AddRamp(NavInputGeometry& g, float x0, float x1, float z0, float z1, float y0, float y1)
{
    AddQuad(g, x0, y0, z0, x1, y0, z0, x1, y1, z1, x0, y1, z1);
}

float PathLength(const std::vector<float>& p)
{
    float len = 0.0f;
    for (size_t i = 0; i + 5 < p.size(); i += 3)
    {
        const float dx = p[i + 3] - p[i + 0];
        const float dy = p[i + 4] - p[i + 1];
        const float dz = p[i + 5] - p[i + 2];
        len += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return len;
}

NavBuildConfig DefaultCfg()
{
    NavBuildConfig c;
    c.cellSize = 0.15f;
    c.cellHeight = 0.10f;
    c.agentHeight = 1.8f;
    c.agentRadius = 0.3f;
    c.agentMaxClimb = 0.4f;
    c.agentMaxSlope = 45.0f;
    c.minRegionArea = 0.5f;
    c.mergeRegionArea = 10.0f;
    return c;
}

} // namespace

int main()
{
    const float ext[3] = { 2.0f, 4.0f, 2.0f };

    // ================= ① 平床 =================
    {
        NavInputGeometry g;
        AddFloor(g, -5, -5, 5, 5, 0.0f);
        g.ComputeBounds();

        NavMesh mesh; NavBuildReport rep;
        const bool ok = BuildNavMesh(g, DefaultCfg(), mesh, rep);
        if (!ok) std::printf("build error: %s\n", rep.error.c_str());
        CHECK(ok);
        CHECK(mesh.PolyCount() > 0);
        // 10x10 の床から半径 0.3 を削ると 9.4x9.4 ≒ 88 m^2 前後
        CHECK(mesh.GetStats().walkableArea > 70.0f);
        CHECK(mesh.GetStats().walkableArea < 100.0f);

        const float s[3] = { -4.0f, 0.0f, -4.0f };
        const float e[3] = { 4.0f, 0.0f, 4.0f };
        std::vector<float> path;
        const int n = mesh.FindPath(s, e, ext, path);
        CHECK(n >= 2);
        // 遮る物が無いのでファネルは 2 点（始点と終点）に畳まれるはず
        CHECK(n <= 3);
        const float straight = std::sqrt(8.0f * 8.0f + 8.0f * 8.0f);
        CHECK(PathLength(path) < straight * 1.05f);
        CHECK(path.size() >= 3);
        if (path.size() >= 3) CHECK(std::fabs(path[1]) < 0.25f);   // 床の高さに乗っている
    }

    // ================= ② 傾斜 ★坂道 =================
    {
        // 30 度の坂は歩ける
        NavInputGeometry g;
        AddFloor(g, -3, -6, 3, 0, 0.0f);
        AddRamp(g, -3, 3, 0, 4, 0.0f, 4.0f * std::tan(30.0f * 3.14159265f / 180.0f));
        g.ComputeBounds();
        NavMesh mesh; NavBuildReport rep;
        CHECK(BuildNavMesh(g, DefaultCfg(), mesh, rep));

        const float s[3] = { 0.0f, 0.0f, -4.0f };
        const float e[3] = { 0.0f, 2.2f, 3.5f };
        std::vector<float> path;
        CHECK(mesh.FindPath(s, e, ext, path) >= 2);
        // 坂の上まで到達している
        CHECK(path.size() >= 6);
        CHECK(path[path.size() - 2] > 1.5f);
    }
    {
        // 70 度の壁のような斜面は歩けない（床だけが残る）
        NavInputGeometry g;
        AddFloor(g, -3, -6, 3, 0, 0.0f);
        AddRamp(g, -3, 3, 0, 4, 0.0f, 4.0f * std::tan(70.0f * 3.14159265f / 180.0f));
        g.ComputeBounds();
        NavMesh mesh; NavBuildReport rep;
        CHECK(BuildNavMesh(g, DefaultCfg(), mesh, rep));

        const float p[3] = { 0.0f, 5.0f, 3.0f };   // 急斜面の途中
        float out[3];
        const float tight[3] = { 0.5f, 0.6f, 0.5f };
        CHECK(mesh.FindNearestPoly(p, tight, out) < 0);
    }

    // ================= ③ 段差 =================
    {
        // 0.3m の段差 → またげる（1 本の経路で繋がる）
        NavInputGeometry g;
        AddFloor(g, -4, -4, 4, 0, 0.0f);
        AddFloor(g, -4, 0, 4, 4, 0.3f);
        AddWallX(g, -4, -4, 4, 0.0f, 0.3f);   // 側面（無くても良いが実物に近づける）
        g.ComputeBounds();
        NavMesh mesh; NavBuildReport rep;
        CHECK(BuildNavMesh(g, DefaultCfg(), mesh, rep));
        const float s[3] = { 0.0f, 0.0f, -3.0f };
        const float e[3] = { 0.0f, 0.3f, 3.0f };
        std::vector<float> path;
        const int n = mesh.FindPath(s, e, ext, path);
        CHECK(n >= 2);
        CHECK(PathLength(path) < 8.0f);       // まっすぐ乗り越えている
    }
    {
        // 1.2m の段差 → またげない（上の面へは繋がらない）
        NavInputGeometry g;
        AddFloor(g, -4, -4, 4, 0, 0.0f);
        AddFloor(g, -4, 0, 4, 4, 1.2f);
        g.ComputeBounds();
        NavMesh mesh; NavBuildReport rep;
        CHECK(BuildNavMesh(g, DefaultCfg(), mesh, rep));
        const float s[3] = { 0.0f, 0.0f, -3.0f };
        const float e[3] = { 0.0f, 1.2f, 3.0f };
        float sp[3], epos[3];
        const int ps = mesh.FindNearestPoly(s, ext, sp);
        const int pe = mesh.FindNearestPoly(e, ext, epos);
        CHECK(ps >= 0 && pe >= 0);
        std::vector<float> path;
        mesh.FindPath(s, e, ext, path);
        // 到達できないので、経路の終点が目的地に届かない
        CHECK(path.size() >= 3);
        const float dx = path[path.size() - 3] - e[0];
        const float dz = path[path.size() - 1] - e[2];
        CHECK(std::sqrt(dx * dx + dz * dz) > 1.0f);
    }

    // ================= ④ エージェント半径 =================
    {
        // 幅 0.4m の通路は半径 0.3m のエージェントには通れない
        NavInputGeometry g;
        AddFloor(g, -4, -4, 4, 4, 0.0f);
        AddWallX(g, -0.2f, -4, 4, 0.0f, 2.0f);
        AddWallX(g, 0.2f, -4, 4, 0.0f, 2.0f);
        // 通路の外は壁で塞ぐ（通路以外の抜け道を作らない）
        AddQuad(g, -0.2f, 0.0f, -4, 0.2f, 0.0f, -4, 0.2f, 2.0f, -4, -0.2f, 2.0f, -4);
        AddQuad(g, -0.2f, 0.0f, 4, 0.2f, 0.0f, 4, 0.2f, 2.0f, 4, -0.2f, 2.0f, 4);
        g.ComputeBounds();
        NavMesh mesh; NavBuildReport rep;
        CHECK(BuildNavMesh(g, DefaultCfg(), mesh, rep));
        const float p[3] = { 0.0f, 0.0f, 0.0f };   // 通路のど真ん中
        const float tight[3] = { 0.15f, 0.5f, 0.15f };
        float out[3];
        CHECK(mesh.FindNearestPoly(p, tight, out) < 0);
    }

    // ================= ⑤ 障害物の回り込み =================
    {
        NavInputGeometry g;
        AddFloor(g, -6, -6, 6, 6, 0.0f);
        // 真ん中に長い壁（両端は開いている）
        AddWallX(g, 0.0f, -4.5f, 4.5f, 0.0f, 2.0f);
        g.ComputeBounds();
        NavMesh mesh; NavBuildReport rep;
        CHECK(BuildNavMesh(g, DefaultCfg(), mesh, rep));

        const float s[3] = { -3.0f, 0.0f, 0.0f };
        const float e[3] = { 3.0f, 0.0f, 0.0f };
        std::vector<float> path;
        const int n = mesh.FindPath(s, e, ext, path);
        CHECK(n >= 3);                          // 折れている
        const float len = PathLength(path);
        CHECK(len > 6.0f);                      // 直線(6m)より長い
        CHECK(len < 20.0f);                     // 大回りしすぎていない
        // 壁を跨いでいないこと（全ての点が壁の端より外側で x を跨ぐ）
        bool crossedInside = false;
        for (size_t i = 0; i + 5 < path.size(); i += 3)
        {
            const float x0 = path[i + 0], z0 = path[i + 2];
            const float x1 = path[i + 3], z1 = path[i + 5];
            if ((x0 < 0.0f) != (x1 < 0.0f))
            {
                const float t = (0.0f - x0) / (x1 - x0);
                const float zc = z0 + (z1 - z0) * t;
                if (zc > -4.5f && zc < 4.5f) crossedInside = true;
            }
        }
        CHECK(!crossedInside);

        // ---- ⑦ レイキャスト ----
        float sp[3];
        const int startPoly = mesh.FindNearestPoly(s, ext, sp);
        CHECK(startPoly >= 0);
        float t = 1.0f, nrm[3], hit[3];
        const float across[3] = { 3.0f, 0.0f, 0.0f };
        CHECK(mesh.Raycast(sp, across, startPoly, t, nrm, hit));   // 壁に当たる
        CHECK(t < 1.0f);
        CHECK(hit[0] < 0.0f);                                      // 壁の手前で止まる
        const float along[3] = { -5.0f, 0.0f, 0.0f };
        float t2 = 1.0f;
        CHECK(!mesh.Raycast(sp, along, startPoly, t2, nrm, hit));  // 開けている
    }

    // ================= ⑥ 坂の高さ取得 =================
    {
        NavInputGeometry g;
        const float slopeY = 6.0f * std::tan(25.0f * 3.14159265f / 180.0f);
        AddRamp(g, -3, 3, -3, 3, 0.0f, slopeY);
        g.ComputeBounds();
        NavMesh mesh; NavBuildReport rep;
        CHECK(BuildNavMesh(g, DefaultCfg(), mesh, rep));

        int sampled = 0;
        float worst = 0.0f;
        for (float z = -2.0f; z <= 2.0f; z += 0.5f)
        {
            const float expect = (z + 3.0f) / 6.0f * slopeY;
            const float p[3] = { 0.0f, expect, z };
            float out[3];
            const int poly = mesh.FindNearestPoly(p, ext, out);
            if (poly < 0) continue;
            float y = 0.0f;
            if (!mesh.GetHeightAt(p, poly, y)) continue;
            worst = (std::max)(worst, std::fabs(y - expect));
            ++sampled;
        }
        CHECK(sampled >= 7);
        // セル高さ(0.10) の 3 倍以内に収まっていれば十分（detail mesh 無しでこの精度）
        CHECK(worst < 0.30f);
        if (worst >= 0.30f) std::printf("  slope height worst error = %.3f\n", worst);
    }

    // ================= ⑧ 保存 / 読み込み =================
    {
        NavInputGeometry g;
        AddFloor(g, -4, -4, 4, 4, 0.0f);
        AddWallX(g, 0.0f, -2.0f, 2.0f, 0.0f, 2.0f);
        g.ComputeBounds();
        NavMesh mesh; NavBuildReport rep;
        CHECK(BuildNavMesh(g, DefaultCfg(), mesh, rep));

        char* tmpEnv = nullptr;
        size_t tmpLen = 0;
        std::string tmpDir = ".";
        if (_dupenv_s(&tmpEnv, &tmpLen, "TEMP") == 0 && tmpEnv) { tmpDir = tmpEnv; std::free(tmpEnv); }
        const std::string path = tmpDir + "/dx12_nav_test.nav";
        std::string err;
        CHECK(mesh.Save(path, err));

        NavMesh loaded;
        CHECK(loaded.Load(path, err));
        CHECK(loaded.PolyCount() == mesh.PolyCount());
        CHECK(loaded.VertCount() == mesh.VertCount());
        CHECK(loaded.GetStats().sampleCount == mesh.GetStats().sampleCount);

        const float s[3] = { -3.0f, 0.0f, 0.0f };
        const float e[3] = { 3.0f, 0.0f, 0.0f };
        std::vector<float> p0, p1;
        const int n0 = mesh.FindPath(s, e, ext, p0);
        const int n1 = loaded.FindPath(s, e, ext, p1);
        CHECK(n0 == n1);
        CHECK(std::fabs(PathLength(p0) - PathLength(p1)) < 1e-3f);
        std::remove(path.c_str());
    }

    std::printf("navmesh_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

