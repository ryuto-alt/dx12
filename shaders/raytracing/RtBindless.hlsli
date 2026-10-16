#ifndef RT_BINDLESS_HLSLI
#define RT_BINDLESS_HLSLI
// RtBindless.hlsli — レイのヒット点で頂点属性とマテリアルを引く（計画09 Step 5）
//
// SM 6.6 の Dynamic Resources（ResourceDescriptorHeap[]）を使う。
// ★必要条件は「SM 6.6」かつ「Resource Binding Tier 3」の 2 つだけで、Agility SDK は要らない
//   （Win11 の in-box ランタイムに入っている。実機の DXC で ps_6_6 コンパイルを実証済み）。
// ★ルートシグネチャに D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED が
//   立っていないと PSO 生成時の検証で落ちる。C++ 側と必ず対で入れること。
//
// ★NonUniformResourceIndex について
//   レイのヒット点由来の添字は「必ず非一様」と仮定する。隣接ピクセルが別のインスタンスに
//   当たるのはフルスクリーン RT では常態なので。付け忘れると結果が未定義になる。
//   逆に、ルート定数由来（テーブル自体のアドレス）は一様なので付けない
//   （付けると無駄な waterfall loop が出る）。

// ★RtCommon.hlsli は include しない。
//   あちらは深度 / SSAO / G-Buffer などスクリーンパス用のバインドを t0.. に宣言するので、
//   compute（DDGI の probe 更新）から使うとレジスタが衝突する。
//   ここはヒット点の読み取りだけを提供し、TLAS と gGeometry のレジスタは includer が決める。

#ifndef RT_GEOMETRY_REGISTER
#define RT_GEOMETRY_REGISTER t6
#endif

// RaytracingScene::GeometryInfo とバイト単位で一致させること（C++ 側に static_assert あり）。
struct GeometryInfo
{
    uint vbSrvIndex;         // 属性用 VB（96B インターリーブ）の raw SRV
    uint ibSrvIndex;         // u32 インデックスバッファの raw SRV
    uint baseColorSrvIndex;  // アルベドテクスチャの SRV。0xFFFFFFFF = 無し
    uint flags;              // bit0 = スキンド / bit1 = 発光テクスチャあり
    // ★S0b: パストレーサーと共有の材質（PtHost::ResolveHitMaterial が解決。8bit 量子化込み）
    float3 tint;             // 色 tint（packedTint の rgb。アルベドに乗る）
    uint emissiveSrvIndex;   // 発光テクスチャの SRV（flags bit1 が立つときだけ有効）
    float3 emissive;         // 放射輝度 = 色 × 強度（テクスチャ前）
    float alphaCutoff;       // RT2: アルファテスト(MASK)のカットオフ（flags bit3 が立つときだけ有効。CSM の ShadowMask と同じ 8bit 量子化値）
    // ★A3 修正: 64B。PT と共有の金属度 / 粗さ（PtHost::ResolveHitMaterial）
    uint  mrSrvIndex;        // MR テクスチャの SRV（flags bit2 が立つときだけ有効。G=粗さ / B=金属度）
    float metallic;
    float roughness;
    uint  pad2;
};
#define RT_GEOM_FLAG_SKINNED     1u
#define RT_GEOM_FLAG_EMISSIVETEX 2u
#define RT_GEOM_FLAG_MRTEX       4u
#define RT_GEOM_FLAG_ALPHATEST   8u   // RT2: MASK 材質。BLAS のジオメトリも非不透明で建っている（候補ループで alpha を見る）

// Mesh.h の Vertex（96B インターリーブ）のバイトオフセット。
// ★ここがズレると全部が静かに壊れる。SkinCompute.hlsl と同じ表。
#define RT_VTX_STRIDE  96
#define RT_VTX_OFS_POS  0
#define RT_VTX_OFS_NOR 12
#define RT_VTX_OFS_COL 24
#define RT_VTX_OFS_UV  40

// GeometryInfo テーブル（ルート SRV）。ヒット点で 1 回だけ引く。
StructuredBuffer<GeometryInfo> gGeometry : register(RT_GEOMETRY_REGISTER);

struct RtHitInfo
{
    bool   valid;
    float3 worldPos;
    float3 worldNormal;   // 補間 + 逆転置でワールドへ
    float2 uv;
    float4 vertexColor;
    uint   baseColorSrvIndex;
    float3 tint;
    float3 emissive;
    uint   emissiveSrvIndex;   // 0xFFFFFFFF = 発光テクスチャ無し
    uint   mrSrvIndex;         // 0xFFFFFFFF = MR テクスチャ無し
    float  metallic;
    float  roughness;
};

// ---------------------------------------------------------------------------
// RT2: アルファテスト(MASK)。DXR 1.1 の any-hit 相当を RayQuery の候補ループで行う。
// ---------------------------------------------------------------------------
// 非不透明（MASK 材質の BLAS ジオメトリ）の候補三角形について、UV を補間して baseColor の α を引き、
// カットオフ以上なら true（＝そこで当たったことにしてよい）。判定式は shaders/shadow/ShadowMask.hlsl の
// clip(a - cutoff) と同じ（a >= cutoff で残る）＝ CSM の影と RT の影で葉の抜け方が一致する。
// ★Sample() は使えない（候補ごとにディスクリプタ添字が発散し LOD が未定義になる）。SampleLevel(…, 0)。
// ★ここは候補（InstanceID = GeometryInfo の添字）を引く。MASK 以外は BLAS が OPAQUE で建っているので
//   そもそも来ないが、来ても true を返す（安全側＝従来と同じ不透明扱い）。
bool RtAlphaTestPass(uint instanceId, uint primIndex, float2 bxy, SamplerState samp)
{
    const GeometryInfo g = gGeometry[instanceId];
    if ((g.flags & RT_GEOM_FLAG_ALPHATEST) == 0u) return true;
    if (g.vbSrvIndex == 0xFFFFFFFFu || g.ibSrvIndex == 0xFFFFFFFFu
        || g.baseColorSrvIndex == 0xFFFFFFFFu)
        return true;   // テクスチャ無し = α は 1 のまま

    ByteAddressBuffer ib = ResourceDescriptorHeap[NonUniformResourceIndex(g.ibSrvIndex)];
    ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(g.vbSrvIndex)];
    const uint3 idx = ib.Load3(primIndex * 12) * RT_VTX_STRIDE;
    const float3 bc = float3(1.0 - bxy.x - bxy.y, bxy.x, bxy.y);
    const float2 uv = asfloat(vb.Load2(idx.x + RT_VTX_OFS_UV)) * bc.x
                    + asfloat(vb.Load2(idx.y + RT_VTX_OFS_UV)) * bc.y
                    + asfloat(vb.Load2(idx.z + RT_VTX_OFS_UV)) * bc.z;
    Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(g.baseColorSrvIndex)];
    return tex.SampleLevel(samp, uv, 0).a >= g.alphaCutoff;
}

// TraceRayInline の後ろで Proceed() を最後まで回す。非不透明の候補は RtAlphaTestPass を通ったものだけ確定する。
// ★RayQuery<flags> はフラグごとに別の型なので関数にできない＝マクロ。呼ぶ場所は RT を撃つ全部
//   （RT 影 / RT-AO / DDGI の更新 / デバッグ）。ACCEPT_FIRST_HIT_AND_END_SEARCH の影レイでも
//   CommitNonOpaqueTriangleHit で確定した時点で探索が終わる。
// ★RAY_FLAG_CULL_NON_OPAQUE / RAY_FLAG_FORCE_OPAQUE をクエリに付けないこと（MASK が消える / 矩形に戻る）。
#define RT_PROCEED_WITH_ALPHA(q, samp)                                                       while ((q).Proceed())                                                                    {                                                                                            if ((q).CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE                                     && RtAlphaTestPass((q).CandidateInstanceID(), (q).CandidatePrimitiveIndex(),                                (q).CandidateTriangleBarycentrics(), (samp)))                         (q).CommitNonOpaqueTriangleHit();                                                }

// ヒット点の属性を読む。RayQuery は CLOSEST_HIT で確定済みであること。
RtHitInfo RtLoadHit(RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q)
{
    RtHitInfo h = (RtHitInfo)0;
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return h;

    // このエンジンは 1 BLAS = 1 geometry（NumDescs=1）なので CommittedGeometryIndex() は
    // 常に 0。InstanceID がそのまま GeometryInfo の添字になる。
    const GeometryInfo g = gGeometry[q.CommittedInstanceID()];
    if (g.vbSrvIndex == 0xFFFFFFFFu || g.ibSrvIndex == 0xFFFFFFFFu)
        return h;

    ByteAddressBuffer ib = ResourceDescriptorHeap[NonUniformResourceIndex(g.ibSrvIndex)];
    ByteAddressBuffer vb = ResourceDescriptorHeap[NonUniformResourceIndex(g.vbSrvIndex)];

    // インデックスは u32 固定なので 1 三角形 = 12 バイト。
    const uint3 idx = ib.Load3(q.CommittedPrimitiveIndex() * 12);
    const uint  b0  = idx.x * RT_VTX_STRIDE;
    const uint  b1  = idx.y * RT_VTX_STRIDE;
    const uint  b2  = idx.z * RT_VTX_STRIDE;

    // ★バリセントリックの順序。DXR 仕様: 「float2.x is the weight for a1 and
    //   float2.y is the weight for a2」＝ a0 の重みは 1-x-y。
    const float2 bxy = q.CommittedTriangleBarycentrics();
    const float3 bc  = float3(1.0 - bxy.x - bxy.y, bxy.x, bxy.y);

    const float2 uv0 = asfloat(vb.Load2(b0 + RT_VTX_OFS_UV));
    const float2 uv1 = asfloat(vb.Load2(b1 + RT_VTX_OFS_UV));
    const float2 uv2 = asfloat(vb.Load2(b2 + RT_VTX_OFS_UV));
    h.uv = uv0 * bc.x + uv1 * bc.y + uv2 * bc.z;

    const float4 c0 = asfloat(vb.Load4(b0 + RT_VTX_OFS_COL));
    const float4 c1 = asfloat(vb.Load4(b1 + RT_VTX_OFS_COL));
    const float4 c2 = asfloat(vb.Load4(b2 + RT_VTX_OFS_COL));
    h.vertexColor = c0 * bc.x + c1 * bc.y + c2 * bc.z;

    // ★スキンドは元 VB（バインドポーズ）の法線なのでアニメーションに追従しない。
    //   位置だけが変形後バッファなので、法線を正しくするには Step 6 以降で
    //   変形後バッファに法線も出すか、変形後の 3 頂点から幾何法線を作る必要がある。
    const float3 n0 = asfloat(vb.Load3(b0 + RT_VTX_OFS_NOR));
    const float3 n1 = asfloat(vb.Load3(b1 + RT_VTX_OFS_NOR));
    const float3 n2 = asfloat(vb.Load3(b2 + RT_VTX_OFS_NOR));
    const float3 nObj = n0 * bc.x + n1 * bc.y + n2 * bc.z;
    // ★法線は逆転置で変換する。mul(row, WorldToObject) が (M^-1)^T·n になるので、
    //   非一様スケールでも正しい。ObjectToWorld を直接掛けるのは誤り。
    h.worldNormal = normalize(mul(nObj, (float3x3)q.CommittedWorldToObject3x4()));

    h.worldPos          = q.WorldRayOrigin() + q.WorldRayDirection() * q.CommittedRayT();
    h.baseColorSrvIndex = g.baseColorSrvIndex;
    h.tint              = g.tint;
    h.emissive          = g.emissive;
    h.emissiveSrvIndex  = ((g.flags & RT_GEOM_FLAG_EMISSIVETEX) != 0u) ? g.emissiveSrvIndex : 0xFFFFFFFFu;
    h.mrSrvIndex        = ((g.flags & RT_GEOM_FLAG_MRTEX) != 0u) ? g.mrSrvIndex : 0xFFFFFFFFu;
    h.metallic          = g.metallic;
    h.roughness         = g.roughness;
    h.valid             = true;
    return h;
}

// ヒット点のアルベド = 頂点カラー × tint × アルベドテクスチャ（パストレーサーの PtEvalMaterial と同じ式）。
// ★Sample() は使えない。quad 内でディスクリプタ添字が発散すると LOD が未定義になる
//   （Learn: "If index diverges across a quad, the hardware-computed derivative and
//     derived quantities such as LOD may be undefined"）。必ず SampleLevel。
float3 RtHitAlbedo(RtHitInfo h, SamplerState samp)
{
    float3 albedo = h.vertexColor.rgb * h.tint;
    if (h.baseColorSrvIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(h.baseColorSrvIndex)];
        albedo *= tex.SampleLevel(samp, h.uv, 0).rgb;
    }
    return albedo;
}

// ヒット点の自己発光（放射輝度）= 色 × 強度 × 発光テクスチャ。無発光なら 0。
float3 RtHitEmissive(RtHitInfo h, SamplerState samp)
{
    float3 em = h.emissive;
    if (any(em > 0.0) && h.emissiveSrvIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(h.emissiveSrvIndex)];
        em *= tex.SampleLevel(samp, h.uv, 0).rgb;
    }
    return em;
}

// ヒット点の金属度 / 粗さ（PtEvalMaterial と同じ式: MR テクスチャの G × roughness / B × metallic。粗さの下限 0.04）。
void RtHitMetalRough(RtHitInfo h, SamplerState samp, out float metallic, out float roughness)
{
    metallic  = h.metallic;
    roughness = h.roughness;
    if (h.mrSrvIndex != 0xFFFFFFFFu)
    {
        Texture2D<float4> tex = ResourceDescriptorHeap[NonUniformResourceIndex(h.mrSrvIndex)];
        const float4 mr = tex.SampleLevel(samp, h.uv, 0);
        roughness = mr.g * h.roughness;
        metallic  = mr.b * h.metallic;
    }
    roughness = max(roughness, 0.04);
}

#endif // RT_BINDLESS_HLSLI
