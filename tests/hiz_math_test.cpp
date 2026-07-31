// Hi-Z オクルージョンカリングの純数学テスト。
//
// このテストが守っているのは「見えている物を消さない（false occlusion を出さない）」の一点。
// 規約を取り違えると全部が静かに反転するので、規約そのものを固定する検査を先頭に置いてある:
//   ・深度は標準 Z（0=near / 1=far）。リバース Z へ移行するなら HiZMath.h と
//     shaders/hiz/*.hlsl の両方を直した上で、このテストの先頭ブロックを書き換えること。
//   ・Hi-Z は 2x2 の **最大値** 縮約。遮蔽は boxMinZ > tileMaxZ。
//
// ミップ選択については「選んだミップの整数テクセル座標で矩形が 2x2 に収まる」という
// 不変条件を乱数で総当たり検証している。ここが崩れると、実際には覆えていない領域の
// 深度を根拠に遮蔽と誤判定して物が消える。

#include "renderer/HiZMath.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdio>

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

constexpr f32 kNear = 0.1f;
constexpr f32 kFar  = 100.0f;

// 原点から +Z を向く標準カメラ。
XMMATRIX MakeViewProj()
{
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, 0, 1),
                                           XMVectorSet(0, 0, 1, 1),
                                           XMVectorSet(0, 1, 0, 0));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0f / 9.0f, kNear, kFar);
    return XMMatrixMultiply(view, proj);
}

HiZViewport FullViewport(f32 w, f32 h)
{
    HiZViewport vp;
    vp.originX = 0.0f; vp.originY = 0.0f; vp.width = w; vp.height = h;
    return vp;
}

ScreenBounds Project(f32 cx, f32 cy, f32 cz, f32 half, const HiZViewport& vp)
{
    return ProjectAabbToScreen(XMVectorSet(cx - half, cy - half, cz - half, 1.0f),
                               XMVectorSet(cx + half, cy + half, cz + half, 1.0f),
                               MakeViewProj(), vp);
}

// 決定的な線形合同法（std::rand の実装差を持ち込まない）。
u32 g_rng = 12345u;
f32 Rand01()
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return static_cast<f32>((g_rng >> 8) & 0xFFFFFFu) / static_cast<f32>(0x1000000u);
}

} // namespace

int main()
{
    const HiZViewport vp = FullViewport(1920.0f, 1080.0f);

    // ── 1. 深度規約の固定 ──────────────────────────────────────────
    // near にある物ほど NDC z が小さい。これが逆になったらリバース Z へ移行したということ。
    {
        const ScreenBounds nearBox = Project(0, 0, 5.0f,  0.5f, vp);
        const ScreenBounds farBox  = Project(0, 0, 50.0f, 0.5f, vp);
        CHECK(nearBox.valid);
        CHECK(farBox.valid);
        CHECK(nearBox.minZ < farBox.minZ);          // 標準 Z: 近い=小さい
        CHECK(nearBox.minZ >= 0.0f && nearBox.minZ <= 1.0f);
        CHECK(farBox.minZ  >= 0.0f && farBox.minZ  <= 1.0f);
    }

    // ── 2. 遮蔽判定の向き ────────────────────────────────────────
    {
        ScreenBounds b;
        b.valid = true; b.minX = 100; b.minY = 100; b.maxX = 110; b.maxY = 110;

        b.minZ = 0.9f;
        CHECK( IsOccludedByHiZ(b, 0.5f));      // 箱(0.9)が壁(0.5)より遠い → 隠れる
        CHECK(!IsOccludedByHiZ(b, 0.95f));     // 箱の方が手前 → 見える

        // 背景（クリア値 1.0）は何も遮蔽しない。ここを弾かないと遠景が全部消える。
        b.minZ = 0.99f;
        CHECK(!IsOccludedByHiZ(b, 1.0f));
        CHECK(!IsOccludedByHiZ(b, 1.5f));      // 想定外の値でも消さない

        // 判定不能は必ず「見える」
        b.valid = false;
        CHECK(!IsOccludedByHiZ(b, 0.0f));

        // NaN は「見える」
        b.valid = true; b.minZ = 0.9f;
        CHECK(!IsOccludedByHiZ(b, std::nanf("")));

        // 等深度（自己遮蔽）は消さない
        b.minZ = 0.5f;
        CHECK(!IsOccludedByHiZ(b, 0.5f));
    }

    // ── 3. 近平面跨ぎ / カメラ内包 / 背後 は必ず valid=false ────────────
    {
        // カメラが箱の内部にいる（原点中心の大きな箱）
        CHECK(!Project(0, 0, 0, 10.0f, vp).valid);
        // 近平面を跨ぐ（z=0.1 の箱が near=0.1 をまたぐ）
        CHECK(!Project(0, 0, 0.1f, 0.5f, vp).valid);
        // 完全に背後
        CHECK(!Project(0, 0, -20.0f, 1.0f, vp).valid);
        // ちょうど near 上
        CHECK(!Project(0, 0, kNear, 0.01f, vp).valid);
    }

    // ── 4. 退化 / NaN ──────────────────────────────────────────────
    {
        const XMMATRIX vpm = MakeViewProj();
        // min > max（反転した AABB）
        CHECK(!ProjectAabbToScreen(XMVectorSet( 1,  1,  10, 1),
                                   XMVectorSet(-1, -1,   9, 1), vpm, vp).valid);
        // NaN 入り
        const f32 nan = std::nanf("");
        CHECK(!ProjectAabbToScreen(XMVectorSet(nan, 0, 10, 1),
                                   XMVectorSet(1,   1, 11, 1), vpm, vp).valid);
        // ゼロサイズのビューポート
        CHECK(!Project(0, 0, 10.0f, 1.0f, FullViewport(0.0f, 1080.0f)).valid);
    }

    // ── 5. 画面外は valid=false（フラスタムカリングの担当） ──────────────
    {
        CHECK(!Project(1000.0f, 0, 10.0f, 0.5f, vp).valid);   // 大きく右
        CHECK(!Project(0, 1000.0f, 10.0f, 0.5f, vp).valid);   // 大きく上
    }

    // ── 6. 矩形がビューポート内へクランプされる ──────────────────────
    {
        const ScreenBounds b = Project(0, 0, 1.0f, 0.4f, vp);   // 手前の大きな箱
        if (b.valid)
        {
            CHECK(b.minX >= 0.0f && b.maxX <= 1920.0f);
            CHECK(b.minY >= 0.0f && b.maxY <= 1080.0f);
            CHECK(b.minX <= b.maxX && b.minY <= b.maxY);
        }
    }

    // ── 7. 画面中央の箱は画面中央に来る（Y 反転の取り違え検出） ────────────
    {
        const ScreenBounds b = Project(0, 0, 20.0f, 0.5f, vp);
        CHECK(b.valid);
        const f32 cx = (b.minX + b.maxX) * 0.5f;
        const f32 cy = (b.minY + b.maxY) * 0.5f;
        CHECK(std::fabs(cx - 960.0f) < 1.0f);
        CHECK(std::fabs(cy - 540.0f) < 1.0f);

        // 上（+Y）にある箱は画面の上半分（小さいピクセル Y）に来る。
        // ここが逆になっていたら NDC→ピクセルの Y 反転を落としている。
        const ScreenBounds up = Project(0, 5.0f, 20.0f, 0.5f, vp);
        CHECK(up.valid);
        CHECK((up.minY + up.maxY) * 0.5f < cy);
    }

    // ── 8. エディタのサブ矩形ビューポート ──────────────────────────
    {
        HiZViewport sub;
        sub.originX = 300.0f; sub.originY = 100.0f; sub.width = 800.0f; sub.height = 600.0f;
        const ScreenBounds b = Project(0, 0, 20.0f, 0.5f, sub);
        CHECK(b.valid);
        // サブ矩形の中央に来ること（フル RT サイズで割っているとここがズレる）
        CHECK(std::fabs((b.minX + b.maxX) * 0.5f - (300.0f + 400.0f)) < 1.0f);
        CHECK(std::fabs((b.minY + b.maxY) * 0.5f - (100.0f + 300.0f)) < 1.0f);
        // 矩形はサブ矩形の外へ出ない
        CHECK(b.minX >= 300.0f && b.maxX <= 1100.0f);
        CHECK(b.minY >= 100.0f && b.maxY <= 700.0f);
    }

    // ── 9. ミップ選択: 小さい矩形は mip 0、大きい矩形ほど上のミップ ────────
    {
        ScreenBounds b;
        b.valid = true; b.minX = 0; b.minY = 0;

        // 1px の矩形は mip 0 のテクセル 1 個に収まる。
        b.maxX = 1;    b.maxY = 1;    CHECK(SelectHiZMip(b, 12) == 0);
        // [0,2] は mip 0 だとテクセル 0/1/2 の 3 個に跨るので mip 1 が要る。
        // 端を含む（inclusive）扱いなのは、切り捨てて覆い漏らすより保守的だから。
        b.maxX = 2;    b.maxY = 2;    CHECK(SelectHiZMip(b, 12) == 1);
        const u32 m8   = (b.maxX = 8,   b.maxY = 8,   SelectHiZMip(b, 12));
        const u32 m64  = (b.maxX = 64,  b.maxY = 64,  SelectHiZMip(b, 12));
        const u32 m512 = (b.maxX = 512, b.maxY = 512, SelectHiZMip(b, 12));
        CHECK(m8 < m64);
        CHECK(m64 < m512);

        // ミップ数でクランプされる
        b.maxX = 100000; b.maxY = 100000;
        CHECK(SelectHiZMip(b, 4) == 3);
        CHECK(SelectHiZMip(b, 1) == 0);
        CHECK(SelectHiZMip(b, 0) == 0);
    }

    // ── 10. ★不変条件: 選んだミップで矩形は 2x2 テクセルに収まる ────────────
    //    これが Hi-Z の正しさの核。崩れると「覆えていない領域の深度」で
    //    遮蔽と誤判定して見えている物が消える。乱数で総当たり検証する。
    {
        constexpr u32 kMipCount = 12;      // 4096 まで
        int violations = 0;
        for (int i = 0; i < 20000; ++i)
        {
            ScreenBounds b;
            b.valid = true;
            b.minX = Rand01() * 1900.0f;
            b.minY = Rand01() * 1060.0f;
            // 1px 〜 画面全体まで、指数的に散らす
            const f32 w = std::pow(2.0f, Rand01() * 11.0f);
            const f32 h = std::pow(2.0f, Rand01() * 11.0f);
            b.maxX = b.minX + w;
            b.maxY = b.minY + h;

            const u32 mip = SelectHiZMip(b, kMipCount);
            CHECK(mip < kMipCount);

            // 最上位ミップまで上げても収まらないのは「ピラミッドが浅い」ケースで、
            // その場合は呼び出し側が遮蔽判定を諦める前提。ここでは最上位以外を検証する。
            if (mip + 1 < kMipCount)
            {
                const f32 inv = 1.0f / static_cast<f32>(1u << mip);
                const int x0 = static_cast<int>(b.minX * inv);
                const int x1 = static_cast<int>(b.maxX * inv);
                const int y0 = static_cast<int>(b.minY * inv);
                const int y1 = static_cast<int>(b.maxY * inv);
                if ((x1 - x0) > 1 || (y1 - y0) > 1) ++violations;
            }
        }
        CHECK(violations == 0);
        if (violations != 0)
            std::printf("  ↑ 2x2 に収まらない矩形が %d 件。false occlusion の原因になる\n", violations);
    }

    // ── 11. 距離が離れるほどスクリーン矩形は小さくなる（投影の健全性） ────────
    {
        const ScreenBounds a = Project(0, 0, 10.0f, 1.0f, vp);
        const ScreenBounds c = Project(0, 0, 40.0f, 1.0f, vp);
        CHECK(a.valid && c.valid);
        CHECK((a.maxX - a.minX) > (c.maxX - c.minX));
    }

    std::printf("hiz_math: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
