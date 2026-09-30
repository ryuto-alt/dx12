// PathTrace.hlsl — DXR パストレーサー(地上真値レンダラ)。inline RayQuery / cs_6_6 / バインドレス。
//
// 進行型(progressive): 1 スレッド = 1 画素 × gSampleCount 本の経路。結果は gAccum に足し込む
// (rgb = 放射輝度の和 / a = サンプル数)。画素 × サンプル番号 × シードだけで乱数が決まる＝決定論。
//
// 光輸送
//   ・直接光 = NEE(太陽 / 点・スポット / エミッシブ三角形 / 環境)。エミッシブ面と環境は BSDF サンプリングとの
//     パワーヒューリスティック MIS。太陽と点・スポットはデルタ光(MIS 無し)。
//   ・多重バウンス(gBounces。1 = 直接光のみ)+ ロシアンルーレット。
//   ・BSDF はフォワードと同じ(PtEvalBrdf)。半透明(Blend)は確率的な通過、Mask はアルファテストで
//     RayQuery の候補ループ内(any-hit 相当)に評価する。
//
// ★bounces の意味: 経路の散乱頂点の数の上限。1 = カメラから見えた面での直接光だけ(NEE + BSDF ヒットの発光)、
//   N = 最大 N-1 回の間接バウンス。

#include "PtCommon.hlsli"

// ---------------------------------------------------------------------------
//  頂点属性の読み出し(Mesh の 96B インターリーブ頂点)
// ---------------------------------------------------------------------------
#define PT_VTX_STRIDE   96
#define PT_VTX_OFS_POS   0
#define PT_VTX_OFS_NOR  12
#define PT_VTX_OFS_COL  24
#define PT_VTX_OFS_UV   40
#define PT_VTX_OFS_TAN  48

float3 PtMulRows(float4 r0, float4 r1, float4 r2, float3 p, float w)
{
    return float3(dot(r0.xyz, p) + r0.w * w, dot(r1.xyz, p) + r1.w * w, dot(r2.xyz, p) + r2.w * w);
}

float3 PtNormalToWorld(PtInstance I, float3 nObj)
{
    // (M^-1)^T n。w2o の行を係数として n の各成分で重み付けして足す。
    return I.w2o0.xyz * nObj.x + I.w2o1.xyz * nObj.y + I.w2o2.xyz * nObj.z;
}

struct PtGeo
{
    float3 pos;       // ワールド位置
    float3 Ng;        // 幾何法線(頂点法線の側へ向ける。正規化)
    float3 Ns;        // 補間した頂点法線(正規化)
    float3 T;         // 接線(ワールド。正規化)
    float  Tw;        // 接線の符号
    float2 uv;
    float4 color;
    float  area;      // ワールド面積
    uint   material;
    bool   skinned;   // スキンド(接線はバインドポーズのままなので法線マップを使わない)
};

PtGeo PtFetchGeo(uint instIdx, uint prim, float2 bxy)
{
    const PtInstance I = gInstances[instIdx];
    ByteAddressBuffer ib = ResourceDescriptorHeap[NonUniformResourceIndex(I.ibSrv)];
    ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(I.vbSrv)];
    const uint3 idx = ib.Load3(prim * 12);
    const uint3 vo  = idx * PT_VTX_STRIDE;
    const float3 bc = float3(1.0 - bxy.x - bxy.y, bxy.x, bxy.y);

    float3 p0, p1, p2;
    const float3 b0p = asfloat(vb.Load3(vo.x + PT_VTX_OFS_POS));
    const float3 b1p = asfloat(vb.Load3(vo.y + PT_VTX_OFS_POS));
    const float3 b2p = asfloat(vb.Load3(vo.z + PT_VTX_OFS_POS));
    const bool skinned = (I.flags & PT_INST_SKINNED) != 0u && I.defVbSrv != PT_NO_INDEX;
    if (skinned)
    {
        ByteAddressBuffer dvb = ResourceDescriptorHeap[NonUniformResourceIndex(I.defVbSrv)];
        p0 = asfloat(dvb.Load3(idx.x * 12));
        p1 = asfloat(dvb.Load3(idx.y * 12));
        p2 = asfloat(dvb.Load3(idx.z * 12));
    }
    else { p0 = b0p; p1 = b1p; p2 = b2p; }

    PtGeo g;
    const float3 pObj = p0 * bc.x + p1 * bc.y + p2 * bc.z;
    g.pos = PtMulRows(I.o2w0, I.o2w1, I.o2w2, pObj, 1.0);

    // 幾何法線・面積(ワールド頂点から直接。向きは下で頂点法線の側へ揃える)
    float3 ngW;
    {
        const float3 w0 = PtMulRows(I.o2w0, I.o2w1, I.o2w2, p0, 1.0);
        const float3 w1 = PtMulRows(I.o2w0, I.o2w1, I.o2w2, p1, 1.0);
        const float3 w2 = PtMulRows(I.o2w0, I.o2w1, I.o2w2, p2, 1.0);
        ngW = cross(w1 - w0, w2 - w0);
        g.area = 0.5 * length(ngW);
    }
    ngW = (dot(ngW, ngW) > 1e-30) ? normalize(ngW) : float3(0, 1, 0);

    // 頂点法線
    const float3 n0 = asfloat(vb.Load3(vo.x + PT_VTX_OFS_NOR));
    const float3 n1 = asfloat(vb.Load3(vo.y + PT_VTX_OFS_NOR));
    const float3 n2 = asfloat(vb.Load3(vo.z + PT_VTX_OFS_NOR));
    float3 nObj = n0 * bc.x + n1 * bc.y + n2 * bc.z;
    if (skinned)
    {
        // 変形後の頂点法線は無い。バインドポーズの補間法線を「バインド面法線 → 変形後の面法線」の
        // 回転で追従させる(滑らかさを保ちつつ向きを合わせる近似)。
        const float3 gb = normalize(cross(b1p - b0p, b2p - b0p) + 1e-20);
        const float3 gd = normalize(cross(p1 - p0, p2 - p0) + 1e-20);
        const float3 axis = cross(gb, gd);
        const float c = dot(gb, gd);
        // Rodrigues(gb → gd)。c ≈ -1 の縮退は無視(法線が反転する面は元々破綻している)
        nObj = nObj * c + cross(axis, nObj) + axis * (dot(axis, nObj) / (1.0 + max(c, -0.999)));
    }
    float3 nsW = PtNormalToWorld(I, nObj);
    nsW = (dot(nsW, nsW) > 1e-30) ? normalize(nsW) : ngW;
    g.Ns = nsW;
    // 幾何法線を頂点法線の側へ揃える(巻き方向に依らず「表」を決める)
    g.Ng = (dot(ngW, nsW) < 0.0) ? -ngW : ngW;

    g.uv = asfloat(vb.Load2(vo.x + PT_VTX_OFS_UV)) * bc.x
         + asfloat(vb.Load2(vo.y + PT_VTX_OFS_UV)) * bc.y
         + asfloat(vb.Load2(vo.z + PT_VTX_OFS_UV)) * bc.z;
    g.color = asfloat(vb.Load4(vo.x + PT_VTX_OFS_COL)) * bc.x
            + asfloat(vb.Load4(vo.y + PT_VTX_OFS_COL)) * bc.y
            + asfloat(vb.Load4(vo.z + PT_VTX_OFS_COL)) * bc.z;

    const float4 t0 = asfloat(vb.Load4(vo.x + PT_VTX_OFS_TAN));
    const float4 t1 = asfloat(vb.Load4(vo.y + PT_VTX_OFS_TAN));
    const float4 t2 = asfloat(vb.Load4(vo.z + PT_VTX_OFS_TAN));
    const float3 tObj = t0.xyz * bc.x + t1.xyz * bc.y + t2.xyz * bc.z;
    float3 tW = PtMulRows(I.o2w0, I.o2w1, I.o2w2, tObj, 0.0);
    g.T  = (dot(tW, tW) > 1e-30) ? normalize(tW) : normalize(cross(g.Ns, float3(0, 1, 0)) + 1e-6);
    g.Tw = (t0.w != 0.0) ? t0.w : 1.0;
    g.material = I.materialIndex;
    g.skinned = skinned;
    return g;
}

// ---------------------------------------------------------------------------
//  材質評価(Forward.hlsl の PSMain と同じ規則)
// ---------------------------------------------------------------------------
float2 PtMatUV(PtMaterial M, float2 uv) { return uv * M.uvScaleOffset.xy + M.uvScaleOffset.zw; }

// PBR.hlsli の PerturbNormal と同じ(接空間 z を 0.35 で打ち止め)
float3 PtPerturbNormal(float3 Ns, float3 T, float tangentW, float2 rg)
{
    const float3 N  = normalize(Ns);
    float3 Tt = T - dot(T, N) * N;
    const float tl2 = dot(Tt, Tt);
    if (tl2 < 1e-10) { float3 bb; PtBasis(N, Tt, bb); }   // 接線が法線と平行 / ゼロ(壊れた UV 展開)。基底を作り直す
    else Tt *= rsqrt(tl2);
    const float3 B  = cross(N, Tt) * tangentW;
    const float2 xy = rg * 2.0 - 1.0;
    const float  z  = sqrt(saturate(1.0 - dot(xy, xy)));
    const float3 tn = normalize(float3(xy, max(z, 0.35)));
    return normalize(tn.x * Tt + tn.y * B + tn.z * N);
}

struct PtMatEval
{
    PtSurf surf;
    float3 N;          // シェーディング法線(ワールド。ノーマルマップ適用後。面の表側を向く前の値)
    float3 emission;
    float  alpha;
};

PtMatEval PtEvalMaterial(PtGeo g)
{
    const PtMaterial M = gMaterials[g.material];
    const float2 uv = PtMatUV(M, g.uv);

    float4 albedo4 = g.color * float4(M.tint, 1.0);
    if (M.albedoSrv != PT_NO_INDEX)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(M.albedoSrv)];
        albedo4 *= t.SampleLevel(gSampWrap, uv, 0);
    }

    PtMatEval e;
    e.alpha = albedo4.a * M.opacity;
    e.N = g.Ns;
    if ((M.flags & PT_MAT_NORMALMAP) != 0u && M.normalSrv != PT_NO_INDEX && (gFlags & PT_FLAG_NO_NORMALMAP) == 0u && !g.skinned)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(M.normalSrv)];
        e.N = PtPerturbNormal(g.Ns, g.T, g.Tw, t.SampleLevel(gSampWrap, uv, 0).rg);
    }

    float metallic = M.metallic, roughness = M.roughness;
    if ((M.flags & PT_MAT_MRTEX) != 0u && M.mrSrv != PT_NO_INDEX)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(M.mrSrv)];
        const float4 mr = t.SampleLevel(gSampWrap, uv, 0);
        roughness = mr.g * M.roughness;
        metallic  = mr.b * M.metallic;
    }
    e.surf.albedo    = albedo4.rgb;
    e.surf.metallic  = metallic;
    e.surf.roughness = max(roughness, 0.04);   // UNO_SHADE_ROUGHNESS
    e.surf.F0        = lerp(float3(0.04, 0.04, 0.04), albedo4.rgb, metallic);
    e.surf.lambert   = ((M.flags & PT_MAT_LAMBERT) != 0u) || ((gFlags & PT_FLAG_FORCE_LAMBERT) != 0u);

    float3 em = M.emissive;
    if (any(em > 0.0) && (M.flags & PT_MAT_EMISSIVETEX) != 0u && M.emissiveSrv != PT_NO_INDEX)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(M.emissiveSrv)];
        em *= t.SampleLevel(gSampWrap, uv, 0).rgb;
    }
    e.emission = em;
    return e;
}

// ---------------------------------------------------------------------------
//  レイ走査(アルファ = any-hit 相当を候補ループで評価)
// ---------------------------------------------------------------------------
bool PtAlphaPass(uint instIdx, uint prim, float2 bxy, inout uint rng)
{
    const PtInstance I = gInstances[instIdx];
    const PtMaterial M = gMaterials[I.materialIndex];
    if (M.alphaMode == 0u) return true;

    ByteAddressBuffer ib = ResourceDescriptorHeap[NonUniformResourceIndex(I.ibSrv)];
    ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(I.vbSrv)];
    const uint3 idx = ib.Load3(prim * 12) * PT_VTX_STRIDE;
    const float3 bc = float3(1.0 - bxy.x - bxy.y, bxy.x, bxy.y);
    const float2 uv = asfloat(vb.Load2(idx.x + PT_VTX_OFS_UV)) * bc.x
                    + asfloat(vb.Load2(idx.y + PT_VTX_OFS_UV)) * bc.y
                    + asfloat(vb.Load2(idx.z + PT_VTX_OFS_UV)) * bc.z;
    const float ca = asfloat(vb.Load(idx.x + PT_VTX_OFS_COL + 12)) * bc.x
                   + asfloat(vb.Load(idx.y + PT_VTX_OFS_COL + 12)) * bc.y
                   + asfloat(vb.Load(idx.z + PT_VTX_OFS_COL + 12)) * bc.z;
    float a = ca;
    if (M.albedoSrv != PT_NO_INDEX)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(M.albedoSrv)];
        a *= t.SampleLevel(gSampWrap, PtMatUV(M, uv), 0).a;
    }
    if (M.alphaMode == 1u) return a >= M.alphaCutoff;   // Mask
    return PtRand(rng) < saturate(a * M.opacity);       // Blend: 確率 = 不透明度で当たり、外れたら素通し
}

struct PtHit
{
    uint   instance;
    uint   prim;
    float2 bary;
    float  t;
};

bool PtTraceClosest(float3 o, float3 d, float tmax, inout uint rng, out PtHit h)
{
    h = (PtHit)0;
    RayDesc r;
    r.Origin = o; r.Direction = d; r.TMin = 0.0; r.TMax = tmax;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            if (PtAlphaPass(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(),
                            q.CandidateTriangleBarycentrics(), rng))
                q.CommitNonOpaqueTriangleHit();
        }
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    h.instance = q.CommittedInstanceID();
    h.prim     = q.CommittedPrimitiveIndex();
    h.bary     = q.CommittedTriangleBarycentrics();
    h.t        = q.CommittedRayT();
    return true;
}

// 遮蔽判定。true = 遮られた。
bool PtOccluded(float3 o, float3 d, float tmax, inout uint rng)
{
    RayDesc r;
    r.Origin = o; r.Direction = d; r.TMin = 0.0; r.TMax = tmax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gTlas, RAY_FLAG_NONE, 0xFF, r);
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            if (PtAlphaPass(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(),
                            q.CandidateTriangleBarycentrics(), rng))
                q.CommitNonOpaqueTriangleHit();
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// ---------------------------------------------------------------------------
//  光源
// ---------------------------------------------------------------------------
// 点/スポットの減衰(ラジアンスへ掛ける倍率)。エンジン式は Lighting.hlsli の AccumulatePunctualLights と同一。
float PtPunctualAtt(PtLight L, float dist, float3 Ldir)
{
    float att;
    if ((gFlags & PT_FLAG_PHYS_FALLOFF) != 0u)
        att = 1.0 / max(dist * dist, 1e-4);
    else
    {
        if (dist >= L.range) return 0.0;
        att = saturate(1.0 - dist / L.range);
        att *= att;
    }
    if (L.type > 0.5)
    {
        const float cd = dot(L.direction, -Ldir);
        float cone = saturate((cd - L.cosOuter) / max(L.cosInner - L.cosOuter, 0.001));
        cone *= cone;
        att *= cone;
    }
    return att;
}

// 選ばれた光の寄与は呼び出し側で計算する(ここは太陽 → 点/スポット → エミッシブ → 環境の順に直接光を足す)。
struct PtShadeCtx
{
    float3 pos;       // 反射点
    float3 Ng;        // 幾何法線(視線側へ向けた)
    float3 N;         // シェーディング法線(視線側へ向けた)
    float3 V;
    PtSurf surf;
    float  pSpec;
};

// 反射点から方向 L へのシャドウレイ始点(幾何法線の L 側へずらす)
float3 PtShadowOrigin(PtShadeCtx c, float3 L)
{
    const float3 n = (dot(c.Ng, L) >= 0.0) ? c.Ng : -c.Ng;
    return PtOffsetRay(c.pos, n);
}

float3 PtDirectLighting(PtShadeCtx c, inout uint rng)
{
    float3 sum = 0.0.xxx;

    // ---- 太陽(円盤光。gSunTanRadius=0 ならデルタ)----
    if (gSunEnabled > 0.5)
    {
        float3 L = gSunToLight;
        if (gSunTanRadius > 0.0)
        {
            // 円錐内の一様な方向(視線に垂直な円盤へ一様に置いて正規化)
            const float2 u = PtRand2(rng);
            const float rr = sqrt(u.x) * gSunTanRadius;
            const float ph = PT_TWO_PI * u.y;
            L = normalize(PtToWorld(float3(rr * cos(ph), rr * sin(ph), 1.0), gSunToLight));
        }
        const float NdotL = dot(c.N, L);
        if (NdotL > 0.0 && dot(c.Ng, L) > 0.0)
        {
            if (!PtOccluded(PtShadowOrigin(c, L), L, 1e30, rng))
                sum += PtEvalBrdf(c.N, c.V, L, c.surf) * NdotL * gSunE;
        }
    }

    // ---- 点/スポット: 期待寄与に比例した重みで 1 灯を選ぶ(1 パスのリザーバ)----
    if (gLightCount > 0u)
    {
        float wSum = 0.0;
        uint  pick = PT_NO_INDEX;
        float pickW = 0.0;
        [loop]
        for (uint i = 0; i < gLightCount; ++i)
        {
            const PtLight L = gLights[i];
            const float3 d = L.position - c.pos;
            const float dist = length(d);
            const float3 Ld = d / max(dist, 1e-4);
            const float NdotL = dot(c.N, Ld);
            if (NdotL <= 0.0 || dot(c.Ng, Ld) <= 0.0) continue;
            const float att = PtPunctualAtt(L, dist, Ld);
            if (att <= 0.0) continue;
            const float w = PtLuma(L.color) * att * NdotL;
            if (w <= 0.0) continue;
            wSum += w;
            if (PtRand(rng) * wSum < w) { pick = i; pickW = w; }
        }
        if (pick != PT_NO_INDEX)
        {
            const PtLight L = gLights[pick];
            const float3 d = L.position - c.pos;
            const float dist = length(d);
            const float3 Ld = d / max(dist, 1e-4);
            const float NdotL = dot(c.N, Ld);
            const float att = PtPunctualAtt(L, dist, Ld);
            if (!PtOccluded(PtShadowOrigin(c, Ld), Ld, dist * 0.999, rng))
            {
                // 推定量 = f * NdotL * color * att / p、p = w / wSum
                const float3 f = PtEvalBrdf(c.N, c.V, Ld, c.surf);
                sum += f * (NdotL * att) * L.color * (wSum / max(pickW, 1e-20));
            }
        }
    }

    // ---- エミッシブ三角形(面積光)+ MIS ----
    if (gEmissiveCount > 0u)
    {
        const float3 ua = float3(PtRand(rng), PtRand(rng), PtRand(rng));
        const float  uf = ua.x * float(gEmissiveCount);
        const uint   k0 = min(uint(uf), gEmissiveCount - 1u);
        const float  fr = uf - float(k0);
        const PtEmissiveTri t0 = gEmissive[k0];
        const uint kk = (fr < t0.thresh) ? k0 : t0.alias;
        const PtEmissiveTri et = gEmissive[kk];
        if (et.pmf > 0.0)
        {
            // 三角形上の一様な点。重み (b0, b1, b2) = (1-su, u2*su, su*(1-u2))。PtFetchGeo へは (b1, b2) を渡す。
            const float su = sqrt(ua.y);
            const float2 bxy = float2(ua.z * su, su * (1.0 - ua.z));
            PtGeo lg = PtFetchGeo(et.instance, et.prim, bxy);
            const PtMaterial LM = gMaterials[lg.material];
            const float3 d = lg.pos - c.pos;
            const float d2 = dot(d, d);
            if (d2 > 1e-10)
            {
                const float dist = sqrt(d2);
                const float3 L = d / dist;
                const float cosL = dot(lg.Ng, -L);
                const bool both = ((gFlags & PT_FLAG_EMISSIVE_BOTH) != 0u) || ((LM.flags & PT_MAT_EMIT_BOTH) != 0u);
                const float cosLa = both ? abs(cosL) : cosL;
                const float NdotL = dot(c.N, L);
                if (cosLa > 1e-5 && NdotL > 0.0 && dot(c.Ng, L) > 0.0)
                {
                    const float pdfLight = et.pmf * d2 / max(et.area * cosLa, 1e-12);
                    if (!PtOccluded(PtShadowOrigin(c, L), L, dist * (1.0 - 1e-3), rng))
                    {
                        const PtMatEval le = PtEvalMaterial(lg);
                        const float pdfB = PtBsdfPdf(c.N, c.V, L, c.surf, c.pSpec);
                        const float mis = PtMisPower(pdfLight, pdfB);
                        sum += PtEvalBrdf(c.N, c.V, L, c.surf) * NdotL * le.emission * (mis / pdfLight);
                    }
                }
            }
        }
    }

    // ---- 環境(コサイン重み方向 + MIS)----
    if ((gFlags & PT_FLAG_ENV_NEE) != 0u)
    {
        const float3 L = PtToWorld(PtCosineHemisphere(PtRand2(rng)), c.N);
        const float NdotL = dot(c.N, L);
        if (NdotL > 1e-5 && dot(c.Ng, L) > 0.0)
        {
            const float pdfE = NdotL * PT_INV_PI;
            if (!PtOccluded(PtShadowOrigin(c, L), L, 1e30, rng))
            {
                const float pdfB = PtBsdfPdf(c.N, c.V, L, c.surf, c.pSpec);
                const float mis = PtMisPower(pdfE, pdfB);
                sum += PtEvalBrdf(c.N, c.V, L, c.surf) * NdotL * PtEnvRadiance(L, gEnvLightScale) * (mis / pdfE);
            }
        }
    }
    return sum;
}

// ---------------------------------------------------------------------------
//  経路
// ---------------------------------------------------------------------------
#ifdef PT_ATMOSPHERE
static float2 gPtPixUv;   // 一次レイの画面 UV(ジッタ込み。大気の AP LUT の引き先)
#endif

float3 PtTracePath(float3 origin, float3 dir, inout uint rng, inout uint rrEnded)
{
    float3 throughput = 1.0.xxx;
    float3 radiance   = 0.0.xxx;
#ifdef PT_ATMOSPHERE
    float3 apL = 0.0.xxx;      // 大気の AP(一次セグメントの in-scattering / 透過率)。無効なら (0, 1)
    float3 apT = 1.0.xxx;
#endif
    float  prevPdf    = 0.0;      // 直前の BSDF サンプルの pdf(0 = 一次レイ)
    float3 prevPos    = origin;
    float3 prevN      = float3(0, 1, 0);
    float3 curOrigin  = origin;
    float3 curDir     = dir;

    [loop]
    for (uint depth = 0; depth <= gBounces; ++depth)
    {
        PtHit hit;
        if (!PtTraceClosest(curOrigin, curDir, 1e30, rng, hit))
        {
            // ミス = 環境。
            if (depth == 0)
            {
#ifdef PT_ATMOSPHERE
                if ((gFlags & PT_FLAG_ATMOSPHERE) != 0u) radiance += throughput * PtAtmosphereBackground(curDir);
                else
#endif
                radiance += throughput * PtEnvRadiance(curDir, gEnvBgScale);
            }
            else
            {
                float w = 1.0;
                if ((gFlags & PT_FLAG_ENV_NEE) != 0u)
                    w = PtMisPower(prevPdf, max(dot(prevN, curDir), 0.0) * PT_INV_PI);
                radiance += throughput * PtEnvRadiance(curDir, gEnvLightScale) * w;
            }
            break;
        }

#ifdef PT_ATMOSPHERE
        // 物理大気(A1)のエアリアルパースペクティブ: 一次レイがジオメトリに当たった距離で froxel LUT を引く(フォワードの PSAp と同じ式)。
        if (depth == 0 && (gFlags & PT_FLAG_ATMOSPHERE) != 0u && (gAtFlags & AT_FLAG_AP) != 0u)
        {
            const float distKm = hit.t * 0.001;
            if (distKm > gApStartKm)
            {
                float3 L1, T1, L0, T0;
                AtSampleAp(gPtPixUv, distKm, L1, T1);
                AtSampleAp(gPtPixUv, gApStartKm, L0, T0);
                const float3 invT0 = 1.0 / max(T0, 1e-4);
                const float3 Lp = max((L1 - L0) * invT0, 0.0);
                const float3 Tp = saturate(T1 * invT0);
                apL = Lp * gLightE * gApStrength;
                apT = lerp(float3(1, 1, 1), Tp, gApStrength);
            }
        }
#endif

        PtGeo g = PtFetchGeo(hit.instance, hit.prim, hit.bary);
        const PtMatEval me = PtEvalMaterial(g);
        const PtMaterial M = gMaterials[g.material];

        // 視線側へ向ける(両面として扱う)。表(Ng の側)から当たったかで発光の有無を決める。
        const bool front = dot(g.Ng, -curDir) > 0.0;
        const float3 Ng = front ? g.Ng : -g.Ng;
        const float3 N  = front ? me.N : -me.N;

        // ---- 発光 ----
        const bool emitBoth = ((gFlags & PT_FLAG_EMISSIVE_BOTH) != 0u) || ((M.flags & PT_MAT_EMIT_BOTH) != 0u);
        if (any(me.emission > 0.0) && (front || emitBoth))
        {
            float w = 1.0;
            const uint eb = gInstances[hit.instance].emissiveBase;
            if (depth > 0 && eb != PT_NO_INDEX && gEmissiveCount > 0u)
            {
                const PtEmissiveTri et = gEmissive[eb + hit.prim];
                const float3 dv = g.pos - prevPos;
                const float d2 = dot(dv, dv);
                const float cosL = abs(dot(g.Ng, curDir));
                const float pdfLight = et.pmf * d2 / max(et.area * cosL, 1e-12);
                w = PtMisPower(prevPdf, pdfLight);
            }
            radiance += throughput * me.emission * w;
        }

        if (depth == gBounces) break;   // 散乱頂点の上限。NEE も BSDF も行わない

        // ---- 反射点の準備 ----
        PtShadeCtx c;
        c.pos  = g.pos;
        c.Ng   = Ng;
        c.N    = N;
        c.V    = -curDir;
        c.surf = me.surf;
        c.pSpec = PtSpecProb(N, c.V, me.surf);

        // ---- 直接光(NEE)----
        radiance += throughput * PtDirectLighting(c, rng);

        // ---- BSDF サンプリング ----
        float3 Ls; float pdf; float3 wgt;
        if (!PtSampleBsdf(N, c.V, me.surf, c.pSpec, rng, Ls, pdf, wgt)) break;
        if (dot(Ng, Ls) <= 0.0) break;   // 幾何的に面の裏へ出る方向は無効(粗いメッシュ + 滑らか法線の保険)
        throughput *= wgt;
        prevPdf = pdf;
        prevPos = g.pos;
        prevN   = N;
        curOrigin = PtOffsetRay(g.pos, Ng);
        curDir    = Ls;

        // ---- ロシアンルーレット ----
        if ((gFlags & PT_FLAG_RR) != 0u && depth + 1 >= gRrStart)
        {
            const float q = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.05, 0.95);
            if (PtRand(rng) > q) { rrEnded = 1u; break; }
            throughput /= q;
        }
    }
#ifdef PT_ATMOSPHERE
    return radiance * apT + apL;
#else
    return radiance;
#endif
}

// ---------------------------------------------------------------------------
//  カメラ
// ---------------------------------------------------------------------------
void PtCameraRay(uint2 px, inout uint rng, out float3 o, out float3 d)
{
    float2 jit = PtRand2(rng);
    if (gPixelFilter > 0.0)
    {
        // 画素フィルタ: 半径 gPixelFilter(画素)の一様円盤ではなく、tent 風に広げる簡易版
        jit = 0.5 + (jit - 0.5) * (1.0 + 2.0 * gPixelFilter);
    }
    const float2 uv = (float2(px) + jit) / float2(gWidth, gHeight);
#ifdef PT_ATMOSPHERE
    gPtPixUv = uv;
#endif
    const float nx = (2.0 * uv.x - 1.0) * gAspect * gTanHalfFovY;
    const float ny = (1.0 - 2.0 * uv.y) * gTanHalfFovY;
    o = gCamPos;
    d = normalize(gCamFwd + gCamRight * nx + gCamUp * ny);
    if (gLensRadius > 0.0)
    {
        // 薄レンズ(被写界深度)。合焦面は gFocusDist(視線方向の距離)
        const float3 focus = gCamPos + d * (gFocusDist / max(dot(d, gCamFwd), 1e-4));
        const float2 u = PtRand2(rng);
        const float r = sqrt(u.x) * gLensRadius;
        const float ph = PT_TWO_PI * u.y;
        o = gCamPos + gCamRight * (r * cos(ph)) + gCamUp * (r * sin(ph));
        d = normalize(focus - o);
    }
}

[numthreads(8, 8, 1)]
void PathTraceCS(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= gTileW || dtid.y >= gTileH) return;
    const uint2 px = uint2(gTileX + dtid.x, gTileY + dtid.y);
    if (px.x >= gWidth || px.y >= gHeight) return;
    const uint pix = px.y * gWidth + px.x;
#ifdef PT_ATMOSPHERE
    PtAtmosphereInit();   // 物理大気(A1)のパラメータ。PT_FLAG_ATMOSPHERE のときだけ読む
#endif

    float3 sum = 0.0.xxx;
    uint nanCount = 0, clampCount = 0, rrCount = 0;
    for (uint s = 0; s < gSampleCount; ++s)
    {
        const uint sampleIndex = gSampleBase + s;
        // 決定論のシード: 画素 × サンプル番号 × ジョブのシード
        uint rng = PtPcg(pix + PtPcg(sampleIndex * 0x9E3779B9u + PtPcg(gSeed + 0x1234567u)));

        float3 o, d;
        PtCameraRay(px, rng, o, d);
        uint rr = 0u;
        float3 L = PtTracePath(o, d, rng, rr);
        rrCount += rr;

        if (any(isnan(L)) || any(isinf(L)))
        {
            ++nanCount;
            L = 0.0.xxx;
        }
        else if (gMaxRadiance > 0.0)
        {
            const float m = max(L.x, max(L.y, L.z));
            if (m > gMaxRadiance) { L *= gMaxRadiance / m; ++clampCount; }
        }
        sum += L;
    }
    const float4 prev = gAccum[pix];
    gAccum[pix] = float4(prev.rgb + sum, prev.a + float(gSampleCount));

    if (gCountRays != 0u)
    {
        if (nanCount)   InterlockedAdd(gStats[0], nanCount);
        if (clampCount) InterlockedAdd(gStats[2], clampCount);
        if (rrCount)    InterlockedAdd(gStats[3], rrCount);
    }
    // 経路数は 64bit(下位 [1] / 上位 [4])。1080p × 数千 spp で 32bit を超える。
    const uint waveSum = WaveActiveSum(gSampleCount);
    if (WaveIsFirstLane())
    {
        uint orig;
        InterlockedAdd(gStats[1], waveSum, orig);
        if (orig + waveSum < orig) InterlockedAdd(gStats[4], 1u);
    }
}
