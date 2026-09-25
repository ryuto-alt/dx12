// 群衆（NavCrowd）の単体テスト。nav/ は純ロジックなので .cpp を直接ビルドしている。
// 実行: ctest --output-on-failure -R NavCrowdTests
//
// ここで守りたい不変条件:
//   ① 1 体が壁を回り込んで目標に着く。どの 1 ステップでもナビメッシュの外に出ない
//   ② 正面衝突する 2 体がすれ違う（重なりは半径の和の 1 割まで）
//   ③ 8 体が同じ点へ集まっても重ならず、全員ナビの上に居る
//   ④ 同じ入力なら同じ結果（ビット単位で一致）＝決定論
//   ⑤ 速度サンプリング 1 回: 正面に障害物があれば避ける向きを選び、無ければ望む向きのまま
//   ⑥ wallMargin を付けると壁ぎわを歩いても壁から離れて歩く
//   ⑦ 速度指定で壁へ突っ込ませてもナビの外へ出ない

#include "nav/NavCrowd.h"
#include "nav/NavTypes.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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
void AddFloor(NavInputGeometry& g, float x0, float z0, float x1, float z1, float y)
{
    AddQuad(g, x0, y, z0, x1, y, z0, x1, y, z1, x0, y, z1);
}
void AddWallX(NavInputGeometry& g, float x, float z0, float z1, float y0, float y1)
{
    AddQuad(g, x, y0, z0, x, y0, z1, x, y1, z1, x, y1, z0);
}
NavBuildConfig Cfg()
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
bool OnMesh(const NavMesh& m, const NavCrowdAgent& ag)
{
    return m.PointInPoly(ag.corridor.FirstPoly(), ag.npos, 1e-3f);
}
float D2(const float* a, const float* b)
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]));
}
} // namespace

int main()
{
    const float dt = 1.0f / 60.0f;

    // 12x12 の床、真ん中に長い壁（両端は開いている）
    NavInputGeometry g;
    AddFloor(g, -6, -6, 6, 6, 0.0f);
    AddWallX(g, 0.0f, -4.5f, 4.5f, 0.0f, 2.0f);
    g.ComputeBounds();
    NavMesh mesh; NavBuildReport rep;
    CHECK(BuildNavMesh(g, Cfg(), mesh, rep));

    // ================= ① 1 体が壁を回り込む =================
    {
        NavCrowd crowd;
        crowd.SetNavMesh(&mesh);
        NavAgentParams p;
        p.radius = 0.3f;
        p.maxSpeed = 3.0f;
        const float s[3] = { -3.0f, 0.0f, 0.0f };
        const float e[3] = { 3.0f, 0.0f, 0.0f };
        const int a = crowd.AddAgent(s, p);
        CHECK(a == 0);
        CHECK(crowd.RequestMoveTarget(a, e));
        int off = 0, steps = 0;
        bool arrived = false;
        for (; steps < 60 * 20; ++steps)
        {
            crowd.Update(dt);
            if (!OnMesh(mesh, *crowd.Agent(a))) ++off;
            if (crowd.Arrived(a, 0.1f)) { arrived = true; break; }
        }
        CHECK(arrived);
        CHECK(off == 0);
        // 経路長 ≒ 2 * sqrt(3^2 + 4.8^2) ≒ 11.3m を 3m/s → 4 秒前後。10 秒以内に着く
        CHECK(steps < 60 * 10);
        CHECK(crowd.Agent(a)->npos[0] > 2.8f);
        CHECK(crowd.GetStats().plans >= 1);
        std::printf("  ① arrived in %.2fs\n", steps * dt);
    }

    // ================= ② 正面衝突する 2 体 =================
    {
        NavCrowd crowd;
        crowd.SetNavMesh(&mesh);
        NavAgentParams p;
        p.radius = 0.4f;
        p.maxSpeed = 2.5f;
        const float a0[3] = { -5.0f, 0.0f, 5.2f }, a1[3] = { -1.0f, 0.0f, 5.2f };
        const float b0[3] = { -1.0f, 0.0f, 5.2f + 0.0f }, b1[3] = { -5.0f, 0.0f, 5.2f };
        (void)b0;
        const int ia = crowd.AddAgent(a0, p);
        const float bs[3] = { -1.2f, 0.0f, 5.2f };
        const int ib = crowd.AddAgent(bs, p);
        CHECK(ia >= 0 && ib >= 0);
        crowd.RequestMoveTarget(ia, a1);
        crowd.RequestMoveTarget(ib, b1);
        float minD = 1e9f;
        int off = 0;
        for (int s = 0; s < 60 * 8; ++s)
        {
            crowd.Update(dt);
            minD = (std::min)(minD, D2(crowd.Agent(ia)->npos, crowd.Agent(ib)->npos));
            if (!OnMesh(mesh, *crowd.Agent(ia)) || !OnMesh(mesh, *crowd.Agent(ib))) ++off;
        }
        CHECK(minD >= (p.radius * 2.0f) * 0.9f);
        CHECK(off == 0);
        CHECK(crowd.Arrived(ia, 0.3f));
        CHECK(crowd.Arrived(ib, 0.3f));
        std::printf("  ② min distance %.3f (radius sum %.2f)\n", minD, p.radius * 2.0f);
    }

    // ================= ③ 8 体が同じ点へ =================
    auto runSwarm = [&](std::vector<float>& outPos, float& outMinD, int& outOff)
    {
        NavCrowd crowd;
        crowd.SetNavMesh(&mesh);
        NavAgentParams p;
        p.radius = 0.35f;
        p.maxSpeed = 3.0f;
        std::vector<int> ids;
        for (int k = 0; k < 8; ++k)
        {
            const float a = static_cast<float>(k) / 8.0f * 6.2831853f;
            const float s[3] = { -3.0f + std::cos(a) * 2.0f, 0.0f, std::sin(a) * 2.0f };
            ids.push_back(crowd.AddAgent(s, p));
        }
        const float goal[3] = { 3.0f, 0.0f, 1.0f };
        for (int id : ids) crowd.RequestMoveTarget(id, goal);
        outMinD = 1e9f;
        outOff = 0;
        for (int s = 0; s < 60 * 12; ++s)
        {
            crowd.Update(dt);
            if (s < 60 * 2) continue;   // 出だしの押し合いは見ない（重なった位置から始めていない）
            for (size_t i = 0; i < ids.size(); ++i)
            {
                if (!OnMesh(mesh, *crowd.Agent(ids[i]))) ++outOff;
                for (size_t j = i + 1; j < ids.size(); ++j)
                    outMinD = (std::min)(outMinD, D2(crowd.Agent(ids[i])->npos, crowd.Agent(ids[j])->npos));
            }
        }
        outPos.clear();
        for (int id : ids) outPos.insert(outPos.end(), crowd.Agent(id)->npos, crowd.Agent(id)->npos + 3);
    };
    {
        std::vector<float> pos1, pos2;
        float minD1 = 0, minD2 = 0;
        int off1 = 0, off2 = 0;
        runSwarm(pos1, minD1, off1);
        runSwarm(pos2, minD2, off2);
        CHECK(off1 == 0);
        CHECK(minD1 >= 0.35f * 2.0f * 0.85f);
        std::printf("  ③ swarm min distance %.3f (radius sum 0.70)\n", minD1);
        // ================= ④ 決定論 =================
        CHECK(pos1.size() == pos2.size());
        CHECK(std::memcmp(pos1.data(), pos2.data(), pos1.size() * sizeof(float)) == 0);
        CHECK(minD1 == minD2);
    }

    // ================= ⑤ 速度サンプリング 1 回 =================
    {
        const float pos[3] = { 0, 0, 0 };
        const float vel[3] = { 2, 0, 0 };
        const float dvel[3] = { 2, 0, 0 };
        std::vector<NavAvoidCircle> circles;
        std::vector<float> segs;
        float out[3];
        // 障害物なし → ほぼ望む速度
        SampleVelocityAdaptive(pos, 0.4f, 2.0f, vel, dvel, circles, segs, NavAvoidPreset(2), out);
        const float ang0 = std::atan2(out[2], out[0]);
        CHECK(std::fabs(ang0) < 0.2f);
        CHECK(std::sqrt(out[0] * out[0] + out[2] * out[2]) > 1.5f);
        // 正面 1.5m に止まっている相手 → 横へ逸れる
        NavAvoidCircle c{};
        c.p[0] = 1.5f; c.rad = 0.4f;
        circles.push_back(c);
        SampleVelocityAdaptive(pos, 0.4f, 2.0f, vel, dvel, circles, segs, NavAvoidPreset(2), out);
        CHECK(std::fabs(out[2]) > 0.2f || out[0] < 0.5f);
        CHECK(std::sqrt(out[0] * out[0] + out[2] * out[2]) <= 2.001f);
        // 正面の壁（線分）→ 壁へ向かう速さが落ちる
        circles.clear();
        segs = { 1.0f, 0.0f, 3.0f, 1.0f, 0.0f, -3.0f };   // x=1 の壁（p→q の左 = -x 側 = 手前）
        SampleVelocityAdaptive(pos, 0.4f, 2.0f, vel, dvel, circles, segs, NavAvoidPreset(2), out);
        CHECK(out[0] < 1.9f);
    }

    // ================= ⑥ wallMargin =================
    {
        NavCrowd crowd;
        crowd.SetNavMesh(&mesh);
        NavAgentParams p;
        p.radius = 0.3f;
        p.maxSpeed = 2.5f;
        p.wallMargin = 0.5f;
        // 壁 x=0 の左の縁（x=-0.3）すれすれを +z へ
        const float s[3] = { -0.4f, 0.0f, -4.0f };
        const float e[3] = { -0.4f, 0.0f, 3.0f };
        const int a = crowd.AddAgent(s, p);
        crowd.RequestMoveTarget(a, e);
        float minWall = 1e9f;
        for (int k = 0; k < 60 * 4; ++k)
        {
            crowd.Update(dt);
            const NavCrowdAgent* ag = crowd.Agent(a);
            if (k > 60 && ag->npos[2] > -3.0f && ag->npos[2] < 2.5f)
                minWall = (std::min)(minWall, -0.3f - ag->npos[0] >= 0.0f ? (-0.3f - ag->npos[0]) : 0.0f);
        }
        // 縁（x=-0.3）から 0.3m 以上離れて歩いている（余白 0.5 に向かって押し返されている）
        CHECK(minWall > 0.3f);
        std::printf("  ⑥ min distance to edge with wallMargin 0.5 = %.3f\n", minWall);
    }

    // ================= ⑦ 速度指定で壁へ突っ込む =================
    {
        NavCrowd crowd;
        crowd.SetNavMesh(&mesh);
        NavAgentParams p;
        p.radius = 0.3f;
        p.maxSpeed = 4.0f;
        const float s[3] = { -1.5f, 0.0f, 0.5f };
        const int a = crowd.AddAgent(s, p);
        const float v[3] = { 4.0f, 0.0f, 0.3f };
        crowd.RequestMoveVelocity(a, v);
        int off = 0, crossed = 0;
        for (int k = 0; k < 60 * 3; ++k)
        {
            crowd.Update(dt);
            if (!OnMesh(mesh, *crowd.Agent(a))) ++off;
            if (crowd.Agent(a)->npos[0] > 0.0f && std::fabs(crowd.Agent(a)->npos[2]) < 4.4f) ++crossed;
        }
        CHECK(off == 0);
        CHECK(crossed == 0);
    }

    std::printf("nav_crowd_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
