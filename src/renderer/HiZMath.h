#pragma once

// Hi-Z（階層深度）オクルージョンカリングの純数学部分。
//
// GPU にも CPU にも依存しない純関数だけを置く（tests/hiz_math_test.cpp で固定）。
// HLSL 側（shaders/hiz/*.hlsl）はここと同じ式を持つ。**片方だけ直すと誤カリングになる**ので、
// 式を変えるときは必ず両方を直し、テストを更新すること。
//
// ── 深度規約（このエンジンの実測事実。変えるときはここも直す）─────────────
//   標準 Z（near=0 / far=1）。リバース Z ではない。根拠は 3 点:
//     1. Camera.cpp が XMMatrixPerspectiveFovLH(fov, aspect, nearZ, farZ) を素通しで使う
//     2. DSV のクリア値が全箇所 1.0f
//     3. 深度比較関数は LESS / LESS_EQUAL のみ（GREATER 系はリポジトリに 0 件）
//   ⇒ 深度が大きいほど遠い。したがって
//     ・Hi-Z ピラミッドは 2x2 の **最大値** で縮約する（＝そのタイルで最も遠い面）
//     ・遮蔽判定は「箱の最も近い点が、タイルの最遠面よりさらに遠い」= boxMinZ > tileMaxZ
//
// ── 保守性の原則 ────────────────────────────────────────────────
//   誤って **見えている物を消す**（false occlusion）のは、多少カリングを取り逃すより
//   遥かに悪い。判断に迷う場合は必ず「見える」を返すこと。この方針で書かれた分岐には
//   すべて理由を書いてある。

#include <DirectXMath.h>

#include "core/Types.h"

namespace dx12e
{

// スクリーン空間へ投影した AABB の外接矩形と、箱の最近深度。
struct ScreenBounds
{
    // ピクセル座標の外接矩形（[minX,maxX] x [minY,maxY]、両端含む）。
    // ビューポート矩形でクランプ済み。
    f32  minX = 0.0f;
    f32  minY = 0.0f;
    f32  maxX = 0.0f;
    f32  maxY = 0.0f;
    // 箱 8 頂点のうち最も手前の NDC 深度（0=near, 1=far）。
    f32  minZ = 0.0f;
    // false = 遮蔽判定にかけてはいけない（近平面を跨ぐ / 画面外 / 退化 / NaN）。
    // このとき呼び出し側は必ず「見える」として扱うこと。
    bool valid = false;
};

// ビューポート（レンダーターゲット内の描画矩形）。
// ★エディタではシーンが RT の一部矩形にしか描かれないので、
//   NDC → ピクセルの変換に必ずこれを使う。フル RT サイズで割ると
//   エディタビューだけカリングがズレる（SSAO.hlsl:31-44 と同じ理由）。
struct HiZViewport
{
    f32 originX = 0.0f;
    f32 originY = 0.0f;
    f32 width   = 1.0f;
    f32 height  = 1.0f;
};

// ワールド空間 AABB を viewProj で投影し、スクリーン矩形と最近深度を得る。
//
// 返り値 valid=false になる条件（どれも「見える」扱いにすべきケース）:
//   ・8 頂点のいずれかが近平面より手前 / 上（w <= kMinW）
//       → 透視除算で符号が反転し、矩形が画面の反対側に化ける。カメラが箱の中に
//         入っている場合もここに落ちる。**これが Hi-Z 実装で最も多いバグ**なので、
//         1 頂点でも該当したら即座に諦める（保守的）。
//   ・矩形がビューポートと交差しない（＝画面外。フラスタムカリングの担当）
//   ・AABB が退化している / NaN が混じっている
inline ScreenBounds ProjectAabbToScreen(DirectX::FXMVECTOR              aabbMin,
                                        DirectX::FXMVECTOR              aabbMax,
                                        DirectX::CXMMATRIX              viewProj,
                                        const HiZViewport&              vp)
{
    using namespace DirectX;

    ScreenBounds out;

    // 近平面跨ぎの閾値。w がこれ以下の頂点が 1 つでもあれば投影を諦める。
    // 0 ではなく小さな正数にするのは、w≈0 で 1/w が発散して矩形が爆発するのを防ぐため。
    constexpr f32 kMinW = 1e-4f;

    const f32 mnx = XMVectorGetX(aabbMin), mny = XMVectorGetY(aabbMin), mnz = XMVectorGetZ(aabbMin);
    const f32 mxx = XMVectorGetX(aabbMax), mxy = XMVectorGetY(aabbMax), mxz = XMVectorGetZ(aabbMax);

    // 退化 / NaN。NaN は比較が常に false になるので、!(a <= b) で拾う。
    if (!(mnx <= mxx) || !(mny <= mxy) || !(mnz <= mxz)) return out;

    if (!(vp.width > 0.0f) || !(vp.height > 0.0f)) return out;

    f32 ndcMinX =  FLT_MAX, ndcMinY =  FLT_MAX, ndcMinZ =  FLT_MAX;
    f32 ndcMaxX = -FLT_MAX, ndcMaxY = -FLT_MAX;

    for (int c = 0; c < 8; ++c)
    {
        const XMVECTOR corner = XMVectorSet((c & 1) ? mxx : mnx,
                                            (c & 2) ? mxy : mny,
                                            (c & 4) ? mxz : mnz,
                                            1.0f);
        // 行ベクトル規約（mul(row, mat)）。エンジン全体でこれに統一されている。
        const XMVECTOR clip = XMVector4Transform(corner, viewProj);

        const f32 w = XMVectorGetW(clip);
        // カメラ位置より後ろ / カメラ平面上 → 諦める（保守的に「見える」）。
        // ★w は「ビュー空間 Z」なので、これは近平面跨ぎの判定にはならない。
        //   near 面の内側にある頂点も w は正（0 < w < nearZ）でここを素通りする。
        //   跨ぎは下の NDC z < 0 で捕まえる。
        if (!(w > kMinW)) return out;

        const f32 invW = 1.0f / w;
        const f32 x = XMVectorGetX(clip) * invW;
        const f32 y = XMVectorGetY(clip) * invW;
        const f32 z = XMVectorGetZ(clip) * invW;

        // NaN 混入（行列に NaN があると全頂点がこれ）。
        if (!(x == x) || !(y == y) || !(z == z)) return out;

        // 近平面跨ぎ（頂点が near より手前）→ 諦める。
        // 跨いだ箱は本来クリップされてから投影されるべきで、頂点をそのまま透視除算した
        // 矩形は実際のシルエットより小さくなり得る＝覆えていない領域の深度で
        // 遮蔽と誤判定する。カメラが箱の内部にいる場合もここに落ちる。
        if (z < 0.0f) return out;

        if (x < ndcMinX) ndcMinX = x;
        if (x > ndcMaxX) ndcMaxX = x;
        if (y < ndcMinY) ndcMinY = y;
        if (y > ndcMaxY) ndcMaxY = y;
        if (z < ndcMinZ) ndcMinZ = z;
    }

    // 最近点が far より遠い＝完全に far の外。フラスタムカリングの担当なので触らない。
    if (ndcMinZ > 1.0f) return out;

    // NDC [-1,1] → ビューポート内ピクセル。Y は D3D なので上下反転。
    const f32 sx0 = vp.originX + (ndcMinX * 0.5f + 0.5f) * vp.width;
    const f32 sx1 = vp.originX + (ndcMaxX * 0.5f + 0.5f) * vp.width;
    // ndcMaxY が画面上端（小さいピクセル Y）に対応する。
    const f32 sy0 = vp.originY + (1.0f - (ndcMaxY * 0.5f + 0.5f)) * vp.height;
    const f32 sy1 = vp.originY + (1.0f - (ndcMinY * 0.5f + 0.5f)) * vp.height;

    const f32 vpMinX = vp.originX, vpMaxX = vp.originX + vp.width;
    const f32 vpMinY = vp.originY, vpMaxY = vp.originY + vp.height;

    // ビューポートと交差しない＝画面外。
    if (sx1 < vpMinX || sx0 > vpMaxX || sy1 < vpMinY || sy0 > vpMaxY) return out;

    out.minX = (sx0 < vpMinX) ? vpMinX : sx0;
    out.minY = (sy0 < vpMinY) ? vpMinY : sy0;
    out.maxX = (sx1 > vpMaxX) ? vpMaxX : sx1;
    out.maxY = (sy1 > vpMaxY) ? vpMaxY : sy1;
    out.minZ = ndcMinZ;
    out.valid = true;
    return out;
}

// スクリーン矩形を 2x2 テクセルで覆える最小のミップを選ぶ。
//
// mipCount はピラミッドのミップ数（mip 0 = 全解像度）。返り値は [0, mipCount-1]。
//
// ★ここが「false occlusion（見えている物が消える）」の主要因。
//   矩形が選んだミップで 2x2 に収まらないと、実際には覆えていない領域の
//   最遠深度を根拠に「隠れている」と誤判定する。
//   よって log2 で概算したあと、**整数テクセル座標で実際に 2x2 に収まるか検算**し、
//   収まらなければミップを 1 段上げる。これで収まることは数学的に保証される
//   （ミップを 1 段上げるとテクセル幅が 2 倍 = 跨ぐテクセル数は必ず減る）。
inline u32 SelectHiZMip(const ScreenBounds& b, u32 mipCount)
{
    if (mipCount == 0) return 0;

    const f32 w = b.maxX - b.minX;
    const f32 h = b.maxY - b.minY;
    f32 maxSide = (w > h) ? w : h;
    if (!(maxSide > 0.0f)) maxSide = 1.0f;   // 退化矩形（1 ピクセル未満）は mip 0 で見る

    // 2 テクセルで覆える最小の mip: 2^mip >= maxSide/2
    i32 mip = 0;
    {
        f32 texel = 2.0f;                     // mip 0 の 2 テクセル幅
        while (texel < maxSide && mip + 1 < static_cast<i32>(mipCount)) { texel *= 2.0f; ++mip; }
    }

    // 整数テクセル座標で 2x2 に収まるか検算（境界跨ぎで 3 テクセルになる場合がある）。
    for (; mip + 1 < static_cast<i32>(mipCount); ++mip)
    {
        const f32 inv = 1.0f / static_cast<f32>(1u << mip);
        const i32 x0 = static_cast<i32>(b.minX * inv);
        const i32 x1 = static_cast<i32>(b.maxX * inv);
        const i32 y0 = static_cast<i32>(b.minY * inv);
        const i32 y1 = static_cast<i32>(b.maxY * inv);
        if ((x1 - x0) <= 1 && (y1 - y0) <= 1) break;
    }

    return static_cast<u32>(mip);
}

// 遮蔽判定の本体。
//   tileMaxDepth = 選んだミップの 2x2 テクセルの **最大値**（＝その範囲で最も遠い面）
// 箱の最近点がそれよりさらに遠ければ、箱は完全にその面の裏 ＝ 遮蔽されている。
//
// ★depthBias の既定は 0。NDC 深度に固定の余裕を足してはいけない。
//   標準 Z + near=0.1 では ndc.z = f/(f-n)*(1 - n/z_view) なので、z_view=10m で既に
//   0.990 に達し、10m〜1000m が [0.990, 1.0] に押し込まれている。つまり同じ NDC の
//   イプシロンが手前ではミリメートル、遠方では数メートルに化ける＝意味を成さない。
//   「ギリギリは見えることにする」は等号の向き（visible ⟺ minZ <= tileMax）で
//   既に担保されている。それでも余裕が要る場合は、深度ではなく
//   **スクリーン矩形を mip0 の 1 テクセルぶん膨らませる**こと（次元的に正しい）。
//   引数を残してあるのは実験用で、常用しない。
inline bool IsOccludedByHiZ(const ScreenBounds& b, f32 tileMaxDepth, f32 depthBias = 0.0f)
{
    if (!b.valid) return false;                  // 判定不能は必ず「見える」
    if (!(tileMaxDepth == tileMaxDepth)) return false;   // NaN
    // tileMaxDepth == 1.0 は「そのタイルには何も描かれていない（クリア値）」＝背景。
    // 背景は何も遮蔽しない。ここを弾かないと far 付近の物が全部消える。
    if (!(tileMaxDepth < 1.0f)) return false;
    return b.minZ > tileMaxDepth + depthBias;
}

} // namespace dx12e
