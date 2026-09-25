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
//   ⑨ FindNearestPoly は【点を含むポリゴン】を返し、outPos はそのポリゴン上の最近点（セル中心へ飛ばない）
//   ⑩ RaycastEx が 4 状態（通れた / 当たった / 始点が外 / 打ち切り）を区別し、
//      ポリゴンの縁・頂点ちょうどから撃っても壁を見落とさない
//   ⑪ MoveAlongSurface は壁を抜けず、着地点がポリゴンの内側（辺から離れている）で、
//      壁ぎわを擦りながら進んでも止まらない
//   ⑫ polyRef は世代つき（焼き直した後の古い ref は無効になる）
//   ⑬ 通路（NavCorridor）: 張る / 角を出す / 位置と目標を滑らせて更新 / 焼き直しで無効
//   ⑭ 通路のマージ規則（Detour と同じ）

#include "nav/NavCorridor.h"
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
        if (PathLength(path) >= straight * 1.05f)
            for (size_t i = 0; i + 2 < path.size(); i += 3)
                std::printf("  p%zu = %.3f %.3f %.3f\n", i / 3, path[i], path[i + 1], path[i + 2]);
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

    // ================= ⑨ FindNearestPoly は点を含むポリゴンを返す =================
    // ★旧実装は高さサンプル格子の「縁のセル = 領域の先頭ポリゴン」をそのまま返していた。
    //   その場合は点を含まないポリゴンが返り、そこから撃ったレイが 1 歩目で失敗していた。
    {
        NavInputGeometry g;
        AddFloor(g, -6, -6, 6, 6, 0.0f);
        AddWallX(g, 0.0f, -4.5f, 4.5f, 0.0f, 2.0f);
        AddWallX(g, -3.0f, -6.0f, -1.5f, 0.0f, 2.0f);
        AddWallX(g, 3.0f, 1.5f, 6.0f, 0.0f, 2.0f);
        g.ComputeBounds();
        NavMesh mesh; NavBuildReport rep;
        CHECK(BuildNavMesh(g, DefaultCfg(), mesh, rep));
        CHECK(mesh.PolyCount() > 4);

        int onMesh = 0, wrongPoly = 0, snapped = 0;
        for (float z = -5.8f; z <= 5.8f; z += 0.037f)
            for (float x = -5.8f; x <= 5.8f; x += 0.041f)
            {
                const float p[3] = { x, 0.0f, z };
                float out[3];
                const int poly = mesh.FindNearestPoly(p, ext, out);
                if (poly < 0) continue;
                const float dx = out[0] - x, dz = out[2] - z;
                if (dx * dx + dz * dz < 1e-10f)
                {
                    ++onMesh;
                    // 点をそのまま返したなら、その点を含むポリゴンでなければならない
                    if (!mesh.PointInPoly(poly, p, 1e-4f)) ++wrongPoly;
                }
                else
                {
                    // 点が外なら、返した点は返したポリゴンの上（縁）にある
                    if (!mesh.PointInPoly(poly, out, 1e-3f)) ++snapped;
                }
            }
        CHECK(onMesh > 1000);
        CHECK(wrongPoly == 0);
        CHECK(snapped == 0);
        if (wrongPoly || snapped) std::printf("  nearest: wrongPoly=%d snapped=%d of %d\n", wrongPoly, snapped, onMesh);

        // 歩行面から 0.1m 外れた点は、セル中心ではなく一番近い縁の点へ落ちる
        {
            // 壁 x=0 の左の縁は x = -0.3（半径）付近。そこから 0.1m 壁寄りの点
            const float p[3] = { -0.18f, 0.0f, 1.234f };
            float out[3];
            const int poly = mesh.FindNearestPoly(p, ext, out);
            CHECK(poly >= 0);
            const float d = std::sqrt((out[0] - p[0]) * (out[0] - p[0]) + (out[2] - p[2]) * (out[2] - p[2]));
            CHECK(d < 0.35f);
            CHECK(std::fabs(out[2] - p[2]) < 0.02f);   // z はほぼそのまま（セル中心へ飛ばない）
            CHECK(out[0] < 0.0f);                      // 壁の向こうへは落ちない
        }

        // ================= ⑩ RaycastEx の 4 状態 =================
        {
            const float s[3] = { -2.0f, 0.0f, 0.3f };
            float sp[3];
            const int sPoly = mesh.FindNearestPoly(s, ext, sp);
            CHECK(sPoly >= 0);
            const float across[3] = { 2.0f, 0.0f, 0.3f };
            NavRaycastResult r = mesh.RaycastEx(sp, across, sPoly);
            CHECK(r.status == NavRayStatus::Hit);
            CHECK(r.t > 0.0f && r.t < 1.0f);
            CHECK(r.hitPos[0] < 0.0f);
            CHECK(r.hitNormal[0] < -0.9f);             // 通路側（-x）を向く法線

            const float open[3] = { -2.0f, 0.0f, -3.0f };
            r = mesh.RaycastEx(sp, open, sPoly);
            CHECK(r.status == NavRayStatus::Clear);
            CHECK(r.t == 1.0f);

            // 始点がポリゴンから大きく外れている → 判定不能（通れた、とは言わない）
            const float far[3] = { 3.0f, 0.0f, 0.3f };
            r = mesh.RaycastEx(far, open, sPoly);
            CHECK(r.status == NavRayStatus::StartOffMesh);
            r = mesh.RaycastEx(sp, open, -1);
            CHECK(r.status == NavRayStatus::StartOffMesh);

            // 打ち切り: 辿れるポリゴン数を 1 に絞って、複数ポリゴンを跨ぐレイを撃つ
            const float s2[3] = { -5.5f, 0.0f, 5.5f };
            const float e2[3] = { -0.6f, 0.0f, -5.5f };
            float sp2[3];
            const int p2 = mesh.FindNearestPoly(s2, ext, sp2);
            CHECK(p2 >= 0);
            std::vector<int> visited;
            r = mesh.RaycastEx(sp2, e2, p2, &visited, 0);
            CHECK(visited.size() > 1);
            if (visited.size() > 1)
            {
                r = mesh.RaycastEx(sp2, e2, p2, nullptr, 1);
                CHECK(r.status == NavRayStatus::Truncated);
            }
            CHECK(std::string(NavRayStatusName(NavRayStatus::Hit)) == "hit");
        }

        // ★縁・頂点ちょうどから壁へ撃っても見落とさない（MikuChase.lua の
        //   「壁が 9.1m 先にあるのに hit=false」の再現）。左半分の全頂点から +x へ撃つ。
        {
            int shots = 0, missed = 0;
            const auto& polys = mesh.Polys();
            for (int pi = 0; pi < mesh.PolyCount(); ++pi)
            {
                const NavPoly& poly = polys[static_cast<size_t>(pi)];
                for (unsigned k = 0; k < poly.vertCount; ++k)
                {
                    float v[3];
                    mesh.GetPolyVert(poly, k, v);
                    if (v[0] > -0.5f || v[0] < -2.9f) continue;           // 壁 x=0 の左、x=-3 の壁の右
                    if (std::fabs(v[2]) > 4.0f) continue;                // 壁の端の回り込みを除く
                    const float s[3] = { v[0], 0.0f, v[2] };
                    float sp[3];
                    const int sPoly = mesh.FindNearestPoly(s, ext, sp);
                    if (sPoly < 0) continue;
                    const float e[3] = { 2.0f, 0.0f, v[2] };
                    const NavRaycastResult r = mesh.RaycastEx(sp, e, sPoly);
                    ++shots;
                    if (r.status != NavRayStatus::Hit || r.hitPos[0] > 0.0f) ++missed;
                    // 旧 API も同じ答え
                    float t = 1.0f, n[3], h[3];
                    if (!mesh.Raycast(sp, e, sPoly, t, n, h)) ++missed;
                }
            }
            CHECK(shots > 3);
            CHECK(missed == 0);
            if (missed) std::printf("  boundary rays missed %d / %d\n", missed, shots);
        }

        // ================= ⑪ MoveAlongSurface =================
        {
            // 壁へ斜めに押し付けながら +z へ進む（毎回位置から探し直す＝Lua の nav.moveAlong と同じ使い方）
            float pos[3] = { -1.0f, 0.0f, -3.5f };
            const float stepX = 0.15f, stepZ = 0.06f;
            int crossed = 0, outside = 0, onEdge = 0;
            const float look[3] = { 0.5f, 2.0f, 0.5f };
            for (int i = 0; i < 120; ++i)
            {
                float sp[3];
                const int poly = mesh.FindNearestPoly(pos, look, sp);
                CHECK(poly >= 0);
                if (poly < 0) break;
                const float to[3] = { sp[0] + stepX, sp[1], sp[2] + stepZ };
                float out[3];
                int outPoly = -1;
                CHECK(mesh.MoveAlongSurface(sp, to, poly, out, outPoly));
                if (out[0] > 0.0f) ++crossed;
                if (!mesh.PointInPoly(outPoly, out, 0.0f)) ++outside;
                std::vector<float> near;
                if (mesh.FindLocalWalls(outPoly, out, 0.002f, near, 4) > 0) ++onEdge;   // 壁から 2mm 未満
                pos[0] = out[0]; pos[1] = out[1]; pos[2] = out[2];
            }
            CHECK(crossed == 0);
            CHECK(outside == 0);
            CHECK(onEdge == 0);
            // 壁ぎわを擦りながらでも z 方向へ進めている（止まらない）: 120 歩 × 0.06 = 7.2m
            CHECK(pos[2] > 1.5f);
            if (pos[2] <= 1.5f) std::printf("  slide stalled at z=%.2f\n", pos[2]);
            CHECK(pos[0] < 0.0f && pos[0] > -0.45f);   // 壁ぎわ（縁）に沿っている

            // 壁の向こうの点を直接狙っても抜けない（旧実装は交差判定失敗でワープしていた）
            const float s[3] = { -0.4f, 0.0f, 0.0f };
            float sp[3];
            const int poly = mesh.FindNearestPoly(s, ext, sp);
            const float to[3] = { 2.0f, 0.0f, 0.0f };
            float out[3];
            int outPoly = -1;
            std::vector<int> visited;
            CHECK(mesh.MoveAlongSurface(sp, to, poly, out, outPoly, &visited));
            CHECK(out[0] < 0.0f);
            CHECK(!visited.empty() && visited.front() == poly && visited.back() == outPoly);
        }

        // ================= ⑫ polyRef =================
        {
            const float p[3] = { -2.0f, 0.0f, 0.0f };
            float out[3];
            const int poly = mesh.FindNearestPoly(p, ext, out);
            const NavPolyRef ref = mesh.MakeRef(poly);
            CHECK(ref != kNullPolyRef);
            CHECK(mesh.DecodeRef(ref) == poly);
            CHECK(mesh.DecodeRef(kNullPolyRef) == -1);
            CHECK(mesh.MakeRef(-1) == kNullPolyRef);

            // 焼き直すと世代が変わり、古い ref は無効になる
            NavMesh again; NavBuildReport rep2;
            CHECK(BuildNavMesh(g, DefaultCfg(), again, rep2));
            CHECK(again.Generation() != mesh.Generation());
            CHECK(again.DecodeRef(ref) == -1);
        }

        // ================= ⑬ 通路 =================
        {
            const float s[3] = { -2.0f, 0.0f, 0.0f };
            const float e[3] = { 2.0f, 0.0f, 0.0f };
            float sp[3];
            const int sPoly = mesh.FindNearestPoly(s, ext, sp);
            NavCorridor c;
            c.Reset(sPoly, sp, mesh.Generation());
            CHECK(c.IsValid(mesh));
            CHECK(c.Plan(mesh, e, ext) == NavPathStatus::Complete);
            CHECK(c.Path().size() >= 2);
            CHECK(c.Path().front() == sPoly);
            const float len0 = c.PathLength(mesh);
            CHECK(len0 > 4.0f);

            // 角へ向かって一定速度で歩く。毎歩 A* をやり直さず、通路だけで目標まで着く
            std::vector<float> corners;
            int steps = 0;
            bool arrived = false;
            for (; steps < 400; ++steps)
            {
                const int n = c.FindCorners(mesh, corners, nullptr, 3);
                CHECK(n >= 1);
                if (n < 1) break;
                const float dx = corners[0] - c.Pos()[0], dz = corners[2] - c.Pos()[2];
                const float dl = std::sqrt(dx * dx + dz * dz);
                const float gx = c.Target()[0] - c.Pos()[0], gz = c.Target()[2] - c.Pos()[2];
                if (std::sqrt(gx * gx + gz * gz) < 0.05f) { arrived = true; break; }
                if (dl < 1e-5f) break;
                const float st = (std::min)(0.1f, dl);
                const float np[3] = { c.Pos()[0] + dx / dl * st, c.Pos()[1], c.Pos()[2] + dz / dl * st };
                CHECK(c.MovePosition(mesh, np));
                CHECK(mesh.PointInPoly(c.FirstPoly(), c.Pos(), 1e-4f));   // 先頭ポリゴンは常に現在地を含む
            }
            if (!arrived)
            {
                for (size_t k = 0; k < c.Path().size() && k < 3; ++k)
                {
                    const NavPoly& pp = mesh.Polys()[static_cast<size_t>(c.Path()[k])];
                    std::printf("  poly %d:", c.Path()[k]);
                    for (unsigned v = 0; v < pp.vertCount; ++v)
                    {
                        float vv[3];
                        mesh.GetPolyVert(pp, v, vv);
                        std::printf(" (%.3f,%.3f)n%d", vv[0], vv[2], static_cast<int>(mesh.Neis()[pp.firstNei + v]));
                    }
                    std::printf("\n");
                }
            }
            CHECK(arrived);
            CHECK(steps < 200);                 // 0.1m/歩 で経路長 +α 程度
            CHECK(c.Path().size() <= 2);        // 通った所は通路から消えている

            // 目標を少し動かす → 末尾を伸ばすだけで届く
            const float e2[3] = { 2.0f, 0.0f, 1.0f };
            CHECK(c.MoveTargetPosition(mesh, e2));
            CHECK(std::fabs(c.Target()[2] - 1.0f) < 0.06f);

            // 角を壁から離す（margin）: 壁の端を回る角が端点から離れる
            NavCorridor c2;
            c2.Reset(sPoly, sp, mesh.Generation());
            c2.Plan(mesh, e, ext);
            std::vector<float> raw, pushed;
            c2.FindCorners(mesh, raw, nullptr, 4, 0.0f);
            c2.FindCorners(mesh, pushed, nullptr, 4, 0.4f);
            CHECK(raw.size() == pushed.size());
            CHECK(raw.size() >= 6);
            if (raw.size() >= 6)
            {
                const float dd = std::sqrt((raw[0] - pushed[0]) * (raw[0] - pushed[0]) +
                                           (raw[2] - pushed[2]) * (raw[2] - pushed[2]));
                CHECK(dd > 0.1f && dd <= 0.41f);
                CHECK(std::fabs(pushed[2]) > std::fabs(raw[2]));   // 壁の端から遠ざかる向き
                if (!(dd > 0.1f && dd <= 0.41f))
                    for (size_t k = 0; k + 2 < raw.size(); k += 3)
                        std::printf("  corner raw=(%.3f,%.3f) pushed=(%.3f,%.3f)\n", raw[k], raw[k + 2], pushed[k], pushed[k + 2]);
            }

            // 焼き直すと通路は無効
            NavMesh again; NavBuildReport rep2;
            CHECK(BuildNavMesh(g, DefaultCfg(), again, rep2));
            CHECK(!c.IsValid(again));
        }
    }

    // ================= ⑭ 通路のマージ規則 =================
    {
        // 前へ進んだ: 通った所が消える
        std::vector<int> path = { 1, 2, 3, 4 };
        MergeCorridorStartMoved(path, { 1, 2, 3 });
        CHECK((path == std::vector<int>{ 3, 4 }));
        // 通路の外（後ろ）へ押し出された: 着地点が先頭に付く
        path = { 1, 2, 3 };
        MergeCorridorStartMoved(path, { 1, 9 });
        CHECK((path == std::vector<int>{ 9, 1, 2, 3 }));
        // 目標が先へ動いた: 末尾が伸びる
        path = { 1, 2, 3 };
        MergeCorridorEndMoved(path, { 3, 7, 8 });
        CHECK((path == std::vector<int>{ 1, 2, 3, 7, 8 }));
        // 目標が戻ってきた: 末尾が縮む
        path = { 1, 2, 3, 4 };
        MergeCorridorEndMoved(path, { 4, 3, 2 });
        CHECK((path == std::vector<int>{ 1, 2 }));
        // 見通せた所まで近道
        path = { 1, 2, 3, 4, 5 };
        MergeCorridorStartShortcut(path, { 1, 6, 4 });
        CHECK((path == std::vector<int>{ 1, 6, 4, 5 }));
    }

    std::printf("navmesh_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

