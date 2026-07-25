// 頂点スカルプト（SculptMeshData）の単体テスト。
// SculptMesh.cpp / TerrainBrush.cpp / HeightField.cpp は純ロジック（GPU も entt も vfs も不要）
// なのでテスト側で直接ビルドしている。実行: ctest --output-on-failure
//
// ここで守りたい不変条件:
//   ① 素体プリミティブの頂点数と「溶接（同位置頂点のグループ化）」が正しい
//   ② 隣接（頂点→頂点）がインデックスから正しく作れている
//   ③ 各ブラシの適用結果（Draw/Smooth/Pinch/Grab/Flatten/Noise + Shift 反転）
//   ④ 法線が「触った所とその隣だけ」作り直される（遠くの頂点は元のまま）
//   ⑤ 対称ミラーが左右で一致する
//   ⑥ .smsh のラウンドトリップ（保存→読み込みで 1 要素もズレない / 壊れた入力を弾く）

#include "terrain/SculptMesh.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace dx12e;
using namespace DirectX;

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

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

bool SamePos(const XMFLOAT3& a, const XMFLOAT3& b, float eps = 1e-5f)
{
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

// 5x5 の板（一辺 4m、原点中心、格子点は整数座標）を作る。
// 頂点 (a,b) の添字は b*5+a、位置は (a-2, 0, b-2)、法線は (0,1,0)。
SculptMeshData MakeTestPlane()
{
    SculptMeshData m;
    m.BuildPrimitive(SculptPrimitive::Plane, 4, 4.0f);
    return m;
}

SculptBrushParams BasicBrush(SculptBrushType type, float radius, float strength)
{
    SculptBrushParams p;
    p.type     = type;
    p.radius   = radius;
    p.strength = strength;
    p.falloff  = 0.0f;      // 縁まで一定（円柱）＝期待値が読める
    return p;
}

} // anonymous namespace

int main()
{
    // ================= ① 素体と溶接 =================
    {
        SculptMeshData empty;
        CHECK(!empty.IsValid());
        CHECK(empty.VertexCount() == 0);

        // 板: 5x5 = 25 頂点、重なる頂点は無いので代表も 25
        const SculptMeshData plane = MakeTestPlane();
        CHECK(plane.IsValid());
        CHECK(plane.VertexCount() == 25u);
        CHECK(plane.RootCount() == 25u);
        CHECK(plane.TriangleCount() == 32u);
        CHECK(SamePos(plane.Positions()[12], XMFLOAT3{0.0f, 0.0f, 0.0f}));
        CHECK(SamePos(plane.Positions()[0],  XMFLOAT3{-2.0f, 0.0f, -2.0f}));
        CHECK(SamePos(plane.Normals()[12],   XMFLOAT3{0.0f, 1.0f, 0.0f}));

        // 箱: 6 面 x 4 頂点 = 24 頂点だが、立方体の角は 8 個しかない
        //     ＝溶接が効いていれば代表頂点は 8（ここが効かないと彫った時に面がバラける）
        SculptMeshData box;
        box.BuildPrimitive(SculptPrimitive::Box, 1, 2.0f);
        CHECK(box.IsValid());
        CHECK(box.VertexCount() == 24u);
        CHECK(box.RootCount() == 8u);
        CHECK(box.TriangleCount() == 12u);

        // 同じ位置の頂点は同じ代表を指す
        for (size_t v = 0; v < box.VertexCount(); ++v)
        {
            const u32 r = box.RootOf(v);
            CHECK(SamePos(box.Positions()[v], box.Positions()[r]));
        }

        // 球・円柱も破綻せず作れる（極/継ぎ目が溶接されるので代表は生頂点より少ない）
        SculptMeshData sphere;
        sphere.BuildPrimitive(SculptPrimitive::Sphere, 8, 2.0f);
        CHECK(sphere.IsValid());
        CHECK(sphere.RootCount() < sphere.VertexCount());
        CHECK(sphere.TriangleCount() > 0);
        // 半径 1（直径 2）の球になっている
        XMFLOAT3 bmin{}, bmax{};
        sphere.Bounds(bmin, bmax);
        CHECK(Near(bmax.x - bmin.x, 2.0f, 0.01f));
        CHECK(Near(bmax.y - bmin.y, 2.0f, 0.01f));

        SculptMeshData cyl;
        cyl.BuildPrimitive(SculptPrimitive::Cylinder, 6, 2.0f);
        CHECK(cyl.IsValid());
        CHECK(cyl.RootCount() < cyl.VertexCount());
    }

    // ================= ② 隣接 =================
    {
        const SculptMeshData plane = MakeTestPlane();
        // 中央 (2,2)=12 の隣は軸方向 4 つ + 三角形分割の対角 2 つ = 6 個。
        // 対角の張り方が変わるとここが 4 or 8 になるので、トポロジの見張り番として効く。
        u32 cnt = 0;
        const u32* nb = plane.NeighborsOf(12, cnt);
        CHECK(nb != nullptr);
        CHECK(cnt == 6u);

        bool hasSelf = false, has0 = false, has24 = false;
        int  axisFound = 0;
        for (u32 i = 0; i < cnt; ++i)
        {
            if (nb[i] == 12) hasSelf = true;
            if (nb[i] == 0)  has0 = true;
            if (nb[i] == 24) has24 = true;
            if (nb[i] == 7 || nb[i] == 11 || nb[i] == 13 || nb[i] == 17) ++axisFound;
        }
        CHECK(!hasSelf);                 // 自分は隣接に入らない
        CHECK(!has0 && !has24);          // 対角の反対側（辺を共有しない）は入らない
        CHECK(axisFound == 4);           // 上下左右は必ず入る

        // 隣接は重複しない（4 頂点ぶんチェック）
        for (u32 root : { 0u, 6u, 12u, 24u })
        {
            u32 c = 0;
            const u32* list = plane.NeighborsOf(root, c);
            for (u32 i = 0; i < c; ++i)
                for (u32 j = i + 1; j < c; ++j)
                    CHECK(list[i] != list[j]);
        }

        // 範囲外は nullptr（落ちない）
        u32 outCnt = 123;
        CHECK(plane.NeighborsOf(99999u, outCnt) == nullptr);
        CHECK(outCnt == 0u);

        // 頂点 → 三角形も張れている（中央は 6 三角形に接する）
        u32 tcnt = 0;
        CHECK(plane.TrianglesOf(12, tcnt) != nullptr);
        CHECK(tcnt == 6u);
    }

    // ================= ③ ブラシ =================
    {
        // --- Draw: 法線方向へ盛る ---
        SculptMeshData m = MakeTestPlane();
        SculptBrushParams p = BasicBrush(SculptBrushType::Draw, 0.5f, 10.0f);
        std::vector<u32> touched;
        CHECK(m.ApplyBrush(p, XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.1f, false, &touched) == 1u);
        CHECK(touched.size() == 1u && touched[0] == 12u);
        CHECK(Near(m.Positions()[12].y, 1.0f));        // strength * dt * w = 10*0.1*1
        CHECK(Near(m.Positions()[11].y, 0.0f));        // 半径外は一切動かない
        CHECK(Near(m.Positions()[12].x, 0.0f));        // 法線方向(+Y)にしか動かない
        CHECK(Near(m.Positions()[12].z, 0.0f));

        // Shift 相当（invert）で戻る
        const XMFLOAT3 peak = m.Positions()[12];
        m.ApplyBrush(p, peak, 0.1f, true, nullptr);
        CHECK(m.Positions()[12].y < 1.0f);

        // dt=0 は何もしない（Grab 以外）
        SculptMeshData m2 = MakeTestPlane();
        CHECK(m2.ApplyBrush(p, XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.0f, false, nullptr) == 0u);
        CHECK(Near(m2.Positions()[12].y, 0.0f));

        // --- Smooth: 突起が隣接の平均へ寄る ---
        SculptMeshData sm = MakeTestPlane();
        sm.SetRootPosition(12, XMFLOAT3{0.0f, 1.0f, 0.0f});
        SculptBrushParams sp = BasicBrush(SculptBrushType::Smooth, 0.5f, 10.0f);
        sm.ApplyBrush(sp, XMFLOAT3{0.0f, 1.0f, 0.0f}, 0.1f, false, nullptr);
        CHECK(Near(sm.Positions()[12].y, 0.0f, 0.01f));   // 周りは全部 y=0 なので平均も 0
        CHECK(Near(sm.Positions()[12].x, 0.0f, 0.01f));   // 対称なので xz は動かない

        // --- Pinch: ブラシ中心へ寄る ---
        SculptMeshData pm = MakeTestPlane();
        SculptBrushParams pp = BasicBrush(SculptBrushType::Pinch, 1.6f, 5.0f);
        pm.ApplyBrush(pp, XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.1f, false, nullptr);
        CHECK(Near(pm.Positions()[13].x, 0.5f, 0.01f));   // (1,0,0) が半分だけ中心へ
        CHECK(Near(pm.Positions()[12].x, 0.0f));          // 中心そのものは動かない
        CHECK(Near(pm.Positions()[14].x, 2.0f));          // (2,0,0) は半径外

        // --- Grab: dt に依らず grabDelta ぶん動く ---
        SculptMeshData gm = MakeTestPlane();
        SculptBrushParams gp = BasicBrush(SculptBrushType::Grab, 0.5f, 1.0f);
        gp.grabDelta = XMFLOAT3{0.0f, 0.5f, 0.0f};
        CHECK(gm.ApplyBrush(gp, XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.0f, false, nullptr) == 1u);
        CHECK(Near(gm.Positions()[12].y, 0.5f));

        // --- Pull: 指定方向へ引っぱる ---
        SculptMeshData um = MakeTestPlane();
        SculptBrushParams up = BasicBrush(SculptBrushType::Pull, 0.5f, 10.0f);
        up.direction = XMFLOAT3{0.0f, 0.0f, 3.0f};   // 長さは無視されて正規化される
        um.ApplyBrush(up, XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.1f, false, nullptr);
        CHECK(Near(um.Positions()[12].z, 1.0f));
        CHECK(Near(um.Positions()[12].y, 0.0f));

        // Push は逆向き
        SculptMeshData hm = MakeTestPlane();
        SculptBrushParams hp = up;
        hp.type = SculptBrushType::Push;
        hm.ApplyBrush(hp, XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.1f, false, nullptr);
        CHECK(Near(hm.Positions()[12].z, -1.0f));

        // --- Flatten: 平均平面へ寄る（持ち上げた頂点が下がる）---
        SculptMeshData fm = MakeTestPlane();
        fm.SetRootPosition(12, XMFLOAT3{0.0f, 1.0f, 0.0f});
        fm.RecomputeAllNormals();
        SculptBrushParams fp = BasicBrush(SculptBrushType::Flatten, 1.6f, 5.0f);
        const float beforeY = fm.Positions()[12].y;
        for (int i = 0; i < 20; ++i)
            fm.ApplyBrush(fp, XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.1f, false, nullptr);
        CHECK(fm.Positions()[12].y < beforeY);
        CHECK(fm.Positions()[12].y >= -0.5f);
        CHECK(Near(fm.Positions()[24].y, 0.0f));   // 半径外は不変

        // --- Noise: 決定的で、同じ引数なら同じ結果 ---
        CHECK(Near(SculptFbm3(XMFLOAT3{1.0f, 2.0f, 3.0f}, 0.5f, 4, 0.0f, 7u),
                   SculptFbm3(XMFLOAT3{1.0f, 2.0f, 3.0f}, 0.5f, 4, 0.0f, 7u), 1e-7f));
        SculptMeshData nm = MakeTestPlane();
        SculptBrushParams np = BasicBrush(SculptBrushType::Noise, 1.6f, 10.0f);
        CHECK(nm.ApplyBrush(np, XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.1f, false, nullptr) == 9u);
        float noiseSum = 0.0f;
        for (size_t v = 0; v < nm.VertexCount(); ++v)
            noiseSum += std::fabs(nm.Positions()[v].y);
        CHECK(noiseSum > 1e-3f);                   // 何かしら凸凹が付いている
        CHECK(Near(nm.Positions()[24].y, 0.0f));   // 半径外は不変

        // --- 溶接グループは必ず一緒に動く（箱の角）---
        SculptMeshData bm;
        bm.BuildPrimitive(SculptPrimitive::Box, 1, 2.0f);
        const XMFLOAT3 corner{1.0f, 1.0f, 1.0f};
        const std::vector<XMFLOAT3> beforePos = bm.Positions();   // コピーを取っておく
        SculptBrushParams bp = BasicBrush(SculptBrushType::Draw, 0.5f, 10.0f);
        bm.ApplyBrush(bp, corner, 0.1f, false, nullptr);
        int movedCount = 0;
        XMFLOAT3 movedPos{0.0f, 0.0f, 0.0f};
        for (size_t v = 0; v < bm.VertexCount(); ++v)
        {
            if (SamePos(beforePos[v], bm.Positions()[v])) continue;
            CHECK(SamePos(beforePos[v], corner));   // 動いたのはブラシを置いた角だけ
            // 角にいた 3 頂点は全部同じ場所へ動いていること（＝面がバラけない）
            if (movedCount == 0) movedPos = bm.Positions()[v];
            else                 CHECK(SamePos(bm.Positions()[v], movedPos));
            ++movedCount;
        }
        // 立方体の角は 3 面が共有＝生頂点 3 個ぶんが一緒に動く
        CHECK(movedCount == 3);
    }

    // ================= ④ 法線の部分再計算 =================
    {
        SculptMeshData m = MakeTestPlane();
        SculptBrushParams p = BasicBrush(SculptBrushType::Draw, 0.5f, 10.0f);
        m.ApplyBrush(p, XMFLOAT3{0.0f, 0.0f, 0.0f}, 0.1f, false, nullptr);

        // (-1,0,0) の頂点は隣が持ち上がったので法線が -X 側へ倒れる
        CHECK(m.Normals()[11].x < -0.05f);
        CHECK(m.Normals()[11].y > 0.0f);
        // 反対側は +X 側へ倒れる（対称）
        CHECK(Near(m.Normals()[13].x, -m.Normals()[11].x, 0.02f));
        // 影響範囲の外は素体の法線のまま（全頂点を舐め直していない証拠）
        CHECK(SamePos(m.Normals()[0],  XMFLOAT3{0.0f, 1.0f, 0.0f}));
        CHECK(SamePos(m.Normals()[24], XMFLOAT3{0.0f, 1.0f, 0.0f}));
        // 山の頂点自体は対称なので真上のまま
        CHECK(Near(m.Normals()[12].y, 1.0f, 0.01f));

        // 全頂点の法線は必ず単位ベクトル
        for (size_t v = 0; v < m.VertexCount(); ++v)
        {
            const XMFLOAT3& n = m.Normals()[v];
            CHECK(Near(std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z), 1.0f, 1e-3f));
        }
    }

    // ================= ⑤ 対称ミラー =================
    {
        // インスタンス生成そのもの
        SculptBrushParams p;
        SculptMeshData::BrushInstance inst[SculptMeshData::kMaxBrushInstances];
        CHECK(SculptMeshData::BuildBrushInstances(p, XMFLOAT3{1.0f, 2.0f, 3.0f}, inst) == 1);
        CHECK(SamePos(inst[0].center, XMFLOAT3{1.0f, 2.0f, 3.0f}));

        p.mirrorX = true;
        CHECK(SculptMeshData::BuildBrushInstances(p, XMFLOAT3{1.0f, 2.0f, 3.0f}, inst) == 2);
        CHECK(SamePos(inst[1].center, XMFLOAT3{-1.0f, 2.0f, 3.0f}));

        p.mirrorY = true;
        p.mirrorZ = true;
        CHECK(SculptMeshData::BuildBrushInstances(p, XMFLOAT3{1.0f, 2.0f, 3.0f}, inst) == 8);

        // 実際に彫った結果が左右で一致する
        SculptMeshData m = MakeTestPlane();
        SculptBrushParams mp = BasicBrush(SculptBrushType::Draw, 0.5f, 10.0f);
        mp.mirrorX = true;
        m.ApplyBrush(mp, XMFLOAT3{1.0f, 0.0f, 0.0f}, 0.1f, false, nullptr);
        CHECK(Near(m.Positions()[13].y, 1.0f));    // ( 1,0,0)
        CHECK(Near(m.Positions()[11].y, 1.0f));    // (-1,0,0) にも同じ山
        CHECK(Near(m.Positions()[13].y, m.Positions()[11].y, 1e-5f));
        CHECK(Near(m.Positions()[12].y, 0.0f));    // 間は半径外なので動かない
    }

    // ================= ⑥ .smsh ラウンドトリップ =================
    {
        SculptMeshData src;
        src.BuildPrimitive(SculptPrimitive::Sphere, 6, 3.0f);
        SculptBrushParams p = BasicBrush(SculptBrushType::Draw, 0.8f, 5.0f);
        src.ApplyBrush(p, XMFLOAT3{1.5f, 0.0f, 0.0f}, 0.2f, false, nullptr);
        CHECK(src.IsValid());

        const std::vector<dx12e::u8> bytes = src.Encode();
        const size_t expect = SculptMeshData::kHeaderSize
                            + src.VertexCount() * (sizeof(XMFLOAT3) * 2 + sizeof(XMFLOAT2))
                            + src.Indices().size() * sizeof(dx12e::u32);
        CHECK(bytes.size() == expect);

        SculptMeshData dst;
        CHECK(dst.Decode(bytes));
        CHECK(dst.VertexCount() == src.VertexCount());
        CHECK(dst.Indices().size() == src.Indices().size());
        CHECK(dst.RootCount() == src.RootCount());   // 溶接/隣接も読み込み後に張り直される

        bool same = true;
        for (size_t v = 0; v < src.VertexCount() && same; ++v)
        {
            if (src.Positions()[v].x != dst.Positions()[v].x
                || src.Positions()[v].y != dst.Positions()[v].y
                || src.Positions()[v].z != dst.Positions()[v].z) same = false;
            if (src.Normals()[v].y != dst.Normals()[v].y) same = false;
            if (src.UVs()[v].x != dst.UVs()[v].x) same = false;
        }
        CHECK(same);   // f32 をそのまま書くので誤差ゼロで一致すること
        for (size_t i = 0; i < src.Indices().size(); ++i)
        {
            if (src.Indices()[i] != dst.Indices()[i]) { CHECK(false); break; }
        }

        // 壊れた入力は弾く（黙って変なメッシュを作らない）
        SculptMeshData bad;
        CHECK(!bad.Decode(nullptr, 0));
        std::vector<dx12e::u8> truncated = bytes;
        truncated.resize(SculptMeshData::kHeaderSize + 8);
        CHECK(!bad.Decode(truncated));
        std::vector<dx12e::u8> wrongMagic = bytes;
        wrongMagic[0] = 0x00;
        CHECK(!bad.Decode(wrongMagic));
        CHECK(!bad.IsValid());

        // BuildFrom は壊れた入力を受け付けない
        const std::vector<XMFLOAT3> pos3{ {0,0,0}, {1,0,0}, {0,0,1} };
        const std::vector<XMFLOAT3> nrm3(3, XMFLOAT3{0.0f, 1.0f, 0.0f});
        const std::vector<XMFLOAT2> uv3(3, XMFLOAT2{0.0f, 0.0f});
        SculptMeshData built;
        CHECK(!built.BuildFrom(pos3, nrm3, uv3, { 0, 1 }));        // 3 の倍数でない
        CHECK(!built.BuildFrom(pos3, nrm3, uv3, { 0, 1, 99 }));    // 範囲外インデックス
        CHECK(built.BuildFrom(pos3, nrm3, uv3, { 0, 1, 2 }));
        CHECK(built.IsValid());
        CHECK(built.VertexCount() == 3u);
        CHECK(built.RootCount() == 3u);
        // 法線/UV を渡さなくても作れる（法線はここで再計算される）
        SculptMeshData built2;
        CHECK(built2.BuildFrom(pos3, {}, {}, { 0, 1, 2 }));
        CHECK(built2.IsValid());
        // (0,0,0)->(1,0,0)->(0,0,1) は cross(v1-v0, v2-v0) = -Y 向き
        CHECK(Near(built2.Normals()[0].y, -1.0f, 1e-3f));
    }

    std::printf("sculpt_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
