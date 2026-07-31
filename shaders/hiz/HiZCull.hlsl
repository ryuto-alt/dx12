// HiZCull.hlsl — Hi-Z によるオクルージョン判定（cs_6_0 / CSMain）。
//
// 1 スレッド = 1 描画アイテム。ワールド AABB をスクリーンへ投影し、
// その矩形を覆う Hi-Z の最遠深度と「箱の最近点」を比べて隠れているかを決める。
//
// ★★ src/renderer/HiZMath.h と式が一対一で対応している。**片方だけ直すと誤カリングになる**。
//    式を変えるときは必ず両方を直し、tests/hiz_math_test.cpp を更新すること。
//    対応:
//      ProjectAabbToScreen  ←→ ProjectAabb()
//      SelectHiZMip         ←→ SelectMip()
//      IsOccludedByHiZ      ←→ CSMain 末尾の比較
//
// ★保守性の原則: 迷ったら必ず「見える」を返す。見えている物を消す(false occlusion)のは
//  多少カリングを取り逃すより遥かに悪い。

cbuffer HiZCullCB : register(b0)
{
    float4x4 gViewProj;    // 深度プリパスと同じ**ジッタ付き** VP。素の VP を使うと半テクセルずれる
    float4   gViewport;    // xy=原点(px) / zw=サイズ(px)。エディタは RT の部分矩形にしか描かない
    float4   gHzbParams;   // xy=mip0 の解像度 / z=ミップ数 / w=アイテム数
};

struct ItemBounds
{
    float4 aabbMin;   // xyz 使用
    float4 aabbMax;   // xyz 使用
};

StructuredBuffer<ItemBounds> g_bounds : register(t0);
Texture2D<float>             g_hzb    : register(t1);
// 1=見える / 0=隠れている。★uint2（8 バイトストライド）なのは D3D12 のプレディケーションが
// 「64bit の値」を要求し、オフセットも 8 バイト境界でなければならないため。
// これでこのバッファをそのまま SetPredication に渡せる（読み戻し不要）。
RWStructuredBuffer<uint2>    g_vis    : register(u0);
RWStructuredBuffer<uint>     g_stats  : register(u1);   // [0]=隠れていた数 / [1]=判定した数

struct ScreenBounds
{
    float4 rect;    // minX, minY, maxX, maxY（ピクセル）
    float  minZ;    // 箱の最も手前の NDC 深度
    bool   valid;
};

// HiZMath.h の ProjectAabbToScreen と同一。
ScreenBounds ProjectAabb(float3 wmin, float3 wmax)
{
    ScreenBounds o;
    o.rect = float4(0, 0, 0, 0);
    o.minZ = 0;
    o.valid = false;

    const float kMinW = 1e-4;

    // 退化 / NaN。NaN は比較が常に false になるので !(a <= b) で拾う。
    if (!(wmin.x <= wmax.x) || !(wmin.y <= wmax.y) || !(wmin.z <= wmax.z)) return o;
    if (!(gViewport.z > 0) || !(gViewport.w > 0)) return o;

    float2 ndcMin = float2( 1e30,  1e30);
    float2 ndcMax = float2(-1e30, -1e30);
    float  ndcMinZ = 1e30;

    [unroll]
    for (int c = 0; c < 8; ++c)
    {
        const float3 p = float3((c & 1) ? wmax.x : wmin.x,
                                (c & 2) ? wmax.y : wmin.y,
                                (c & 4) ? wmax.z : wmin.z);
        // 行ベクトル規約（mul(row, mat)）。エンジン全体でこれに統一されている。
        const float4 clip = mul(float4(p, 1.0), gViewProj);

        // カメラ位置より後ろ / カメラ平面上 → 諦める（保守的に「見える」）。
        // ★w はビュー空間 Z なので、これは近平面跨ぎの判定にはならない。跨ぎは下の z<0 で捕まえる。
        if (!(clip.w > kMinW)) return o;

        const float3 ndc = clip.xyz / clip.w;

        // NaN 混入
        if (!(ndc.x == ndc.x) || !(ndc.y == ndc.y) || !(ndc.z == ndc.z)) return o;

        // 近平面跨ぎ。頂点をそのまま透視除算した矩形は実際のシルエットより小さくなり得るので
        // 諦める。カメラが箱の内部にいる場合もここに落ちる。
        if (ndc.z < 0.0) return o;

        ndcMin = min(ndcMin, ndc.xy);
        ndcMax = max(ndcMax, ndc.xy);
        ndcMinZ = min(ndcMinZ, ndc.z);
    }

    // 最近点が far より遠い＝完全に far の外。フラスタムカリングの担当。
    if (ndcMinZ > 1.0) return o;

    // NDC [-1,1] → ビューポート内ピクセル。Y は D3D なので上下反転
    // （ndcMax.y が画面上端＝小さいピクセル Y に対応する）。
    const float sx0 = gViewport.x + (ndcMin.x * 0.5 + 0.5) * gViewport.z;
    const float sx1 = gViewport.x + (ndcMax.x * 0.5 + 0.5) * gViewport.z;
    const float sy0 = gViewport.y + (1.0 - (ndcMax.y * 0.5 + 0.5)) * gViewport.w;
    const float sy1 = gViewport.y + (1.0 - (ndcMin.y * 0.5 + 0.5)) * gViewport.w;

    const float vpMinX = gViewport.x, vpMaxX = gViewport.x + gViewport.z;
    const float vpMinY = gViewport.y, vpMaxY = gViewport.y + gViewport.w;

    if (sx1 < vpMinX || sx0 > vpMaxX || sy1 < vpMinY || sy0 > vpMaxY) return o;   // 画面外

    o.rect  = float4(max(sx0, vpMinX), max(sy0, vpMinY),
                     min(sx1, vpMaxX), min(sy1, vpMaxY));
    o.minZ  = ndcMinZ;
    o.valid = true;
    return o;
}

// HiZMath.h の SelectHiZMip と同一。
// 矩形を 2x2 テクセルで覆える最小のミップ。log2 の概算だけでは境界跨ぎで 3 テクセルに
// 広がるので、整数テクセル座標で検算して足りなければ 1 段上げる。
uint SelectMip(float4 rect, uint mipCount)
{
    if (mipCount == 0) return 0;

    const float w = rect.z - rect.x;
    const float h = rect.w - rect.y;
    float maxSide = max(w, h);
    if (!(maxSide > 0.0)) maxSide = 1.0;

    int mip = 0;
    {
        float texel = 2.0;
        [loop]
        while (texel < maxSide && mip + 1 < (int)mipCount) { texel *= 2.0; ++mip; }
    }

    [loop]
    for (; mip + 1 < (int)mipCount; ++mip)
    {
        const float inv = 1.0 / (float)(1u << mip);
        const int x0 = (int)(rect.x * inv);
        const int x1 = (int)(rect.z * inv);
        const int y0 = (int)(rect.y * inv);
        const int y1 = (int)(rect.w * inv);
        if ((x1 - x0) <= 1 && (y1 - y0) <= 1) break;
    }
    return (uint)mip;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint i = dtid.x;
    const uint itemCount = (uint)gHzbParams.w;
    if (i >= itemCount) return;

    const ItemBounds b = g_bounds[i];
    const ScreenBounds sb = ProjectAabb(b.aabbMin.xyz, b.aabbMax.xyz);

    if (!sb.valid)
    {
        g_vis[i] = uint2(1, 0);  // 判定不能は必ず「見える」
        uint d;
        InterlockedAdd(g_stats[1], 1u, d);
        return;
    }

    const uint mipCount = (uint)gHzbParams.z;
    const uint mip = SelectMip(sb.rect, mipCount);

    // 選んだミップのテクセル座標へ。矩形の 4 隅を引いて最大値を取る
    // （2x2 に収まることは SelectMip が保証している）。
    const float  inv     = 1.0 / (float)(1u << mip);
    const int2   mipSize = max(int2(1, 1), int2(gHzbParams.xy * inv));
    const int2   t0      = clamp(int2(sb.rect.xy * inv), int2(0, 0), mipSize - 1);
    const int2   t1      = clamp(int2(sb.rect.zw * inv), int2(0, 0), mipSize - 1);

    // ★点サンプルのみ。線形サンプラで混ぜると真の max より小さい値が返り、
    //   遮蔽と判定されやすくなる＝見えている物が消える。
    float hzbMax = g_hzb.Load(int3(t0.x, t0.y, mip));
    hzbMax = max(hzbMax, g_hzb.Load(int3(t1.x, t0.y, mip)));
    hzbMax = max(hzbMax, g_hzb.Load(int3(t0.x, t1.y, mip)));
    hzbMax = max(hzbMax, g_hzb.Load(int3(t1.x, t1.y, mip)));

    // HiZMath.h の IsOccludedByHiZ と同一。
    //  ・NaN は「見える」
    //  ・1.0 は「そのタイルには何も描かれていない（クリア値）＝背景」。背景は何も遮蔽しない。
    //    ここを弾かないと far 付近の物が全部消える
    //  ・NDC 深度に固定のイプシロンを足さない（標準 Z では手前と遠方で意味が変わるため）。
    //    等号は「見える」側に倒す
    bool occluded = false;
    if ((hzbMax == hzbMax) && (hzbMax < 1.0))
        occluded = sb.minZ > hzbMax;

    g_vis[i] = uint2(occluded ? 0u : 1u, 0u);

    uint dummy;
    if (occluded) InterlockedAdd(g_stats[0], 1u, dummy);
    InterlockedAdd(g_stats[1], 1u, dummy);
}
