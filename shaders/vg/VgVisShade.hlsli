// VgVisShade.hlsli ― 仮想ジオメトリ P4: 可視性バッファ 1 画素から「三角形・重心座標・属性・1 画素差分」を復元する共通部品。
//
//   使う側: VgGBuffer.hlsl（VG の RS。速度 + G-Buffer）/ VgResolve.hlsl（メイン RS。材質 + ライティング）/ VgDebug.hlsl（可視化）。
//   ★cbuffer もリソース宣言も持たない（添字は引数で受ける。b0 / b1 の意味が RS ごとに違うため）。
//
// ■重心座標（透視補正）と画面空間微分
//   3 頂点のクリップ座標 c_i から 2D 同次座標 v_i = (x_i, y_i, w_i) を作り、M = [v0 v1 v2]（列）の逆行列の行
//   e_i = cross(v_j, v_k) / det を使う（Olano & Greer 1997）。NDC の点 p に対して L_i(p) = e_i · (p.x, p.y, 1) は
//   「属性 / w」を線形補間する関数で、透視補正の重心座標は λ_i = L_i / ΣL。det は正規化で消えるので割らない。
//   ★NDC へ割ってから 2D の重心を取る方式（The Forge 等）と数学的に同じだが、頂点の w が 0 以下（近平面をまたぐ大きな三角形）
//     でも割り算が無いので破綻しない。
//   「隣の画素（x+1 / y+1）での λ」は L に e_i.x * (2/W)（y は −2/H）を足して正規化し直すだけ＝HW の ddx_fine と同じ
//   1 画素差分（同じ三角形の平面上へ外挿）。UV / 位置 / 法線の勾配はこの差分の線形結合。テクスチャは SampleGrad に渡す。
//   CPU 参照: tests/vg_resolve_gpu_test.cpp（同じ式を double で計算して GPU と突き合わせる）。
//
// ■頂点属性（.vgeo の頂点ブロック。docs/VGEO_SPEC.md §5 / VgeoFormat.h の DecodeCluster と同じ式）
//   位置 = origin + q * step（VgeoDecode.hlsli）/ 法線 = oct16（snorm16 × 2）/ UV = uvBase + (u16 / 65535) * uvScale。
#ifndef VG_VIS_SHADE_HLSLI
#define VG_VIS_SHADE_HLSLI

#include "VgTypes.hlsli"
#include "VgeoDecode.hlsli"

struct VgVisSurface
{
    uint   instance;       // VgInstances の添字
    uint   clusterRef;     // page | clusterInPage << 16
    uint   slot;           // 可視スロット
    uint   tri;            // クラスタ内の三角形番号
    uint   materialIndex;  // アセット内の材質番号（1 クラスタ = 1 材質）
    uint   level;          // DAG のレベル（0 = LOD0）
    VgInstance inst;
    VgAssetGpu asset;
    float3 bary;           // 透視補正の重心座標 λ0..2
    float3 baryDx;         // x + 1 画素での λ − λ
    float3 baryDy;         // y + 1 画素での λ − λ
    float3 lp0, lp1, lp2;  // 3 頂点の位置（アセット空間）
    float3 w0, w1, w2;     // 3 頂点の位置（ワールド）
    float3 n0, n1, n2;     // 3 頂点の法線（ワールド・正規化済み = フォワード VS の normalize(mul(n, (float3x3)model)) と同じ）
    float2 t0, t1, t2;     // 3 頂点の UV（材質の uvScaleOffset を掛ける前）
    float3 localPos;       // 補間したアセット空間の位置（速度の前フレーム位置に使う）
    float3 worldPos;       // 補間したワールド位置
    float3 worldPosDx;     // ワールド位置の 1 画素差分
    float3 worldPosDy;
    float3 worldNormal;    // 補間したワールド法線（未正規化 = フォワードの PSInput.worldNormal と同じ意味）
    float2 uv;             // 補間した UV
    float2 uvDx;           // UV の 1 画素差分
    float2 uvDy;
};

// 法線（oct16: 下位 16bit = x, 上位 16bit = y の snorm16）。VgeoFormat.h の DecodeOct16 と同じ式。
float3 VgDecodeOct16(uint p)
{
    float x = max((float)((int)(p << 16) >> 16) / 32767.0, -1.0);
    float y = max((float)((int)p >> 16) / 32767.0, -1.0);
    const float z = 1.0 - abs(x) - abs(y);
    if (z < 0.0)
    {
        const float nx = (1.0 - abs(y)) * (x >= 0.0 ? 1.0 : -1.0);
        const float ny = (1.0 - abs(x)) * (y >= 0.0 ? 1.0 : -1.0);
        x = nx; y = ny;
    }
    return normalize(float3(x, y, z));
}

// インスタンス行列で方向ベクトルを変換（mul(n, (float3x3)model) と同じ）
float3 VgXformDir(VgInstance inst, float3 n)
{
    return float3(dot(n, inst.w0.xyz), dot(n, inst.w1.xyz), dot(n, inst.w2.xyz));
}

// 透視補正の重心座標と 1 画素差分。c0..c2 = クリップ座標（ラスタと同じ VP）、svPos = SV_Position.xy（画素中心）、
// vp = (幅, 高さ)。戻り値 false = 退化（画面上で面積 0）。
bool VgBarycentrics(float4 c0, float4 c1, float4 c2, float2 svPos, float2 vp,
                    out float3 bary, out float3 baryDx, out float3 baryDy)
{
    const float3 v0 = float3(c0.x, c0.y, c0.w);
    const float3 v1 = float3(c1.x, c1.y, c1.w);
    const float3 v2 = float3(c2.x, c2.y, c2.w);
    const float3 e0 = cross(v1, v2);
    const float3 e1 = cross(v2, v0);
    const float3 e2 = cross(v0, v1);
    const float3 p = float3(svPos.x / vp.x * 2.0 - 1.0, 1.0 - svPos.y / vp.y * 2.0, 1.0);
    const float3 L  = float3(dot(e0, p), dot(e1, p), dot(e2, p));
    const float3 dx = float3(e0.x, e1.x, e2.x) * (2.0 / vp.x);
    const float3 dy = float3(e0.y, e1.y, e2.y) * (-2.0 / vp.y);
    const float s  = L.x + L.y + L.z;
    const float3 Lx = L + dx;
    const float3 Ly = L + dy;
    const float sx = Lx.x + Lx.y + Lx.z;
    const float sy = Ly.x + Ly.y + Ly.z;
    const bool ok = (s != 0.0) && (sx != 0.0) && (sy != 0.0);
    bary   = ok ? L / s : float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
    baryDx = ok ? Lx / sx - bary : float3(0, 0, 0);
    baryDy = ok ? Ly / sy - bary : float3(0, 0, 0);
    return ok;
}

// 可視性バッファの値 vis の画素（svPos）の面を復元する。vis が空なら false。
//   workUav = 作業バッファ（RWByteAddressBuffer）/ visibleOff = 可視リストのバイト位置 / instSrv, assetSrv = 構造化バッファ
//   vpJ = ラスタと同じジッタ付き VP（mul(float4(p,1), vpJ)）/ vp = レンダー解像度
bool VgReconstruct(uint vis, float2 svPos, float4x4 vpJ, float2 vp, uint workUav, uint visibleOff, uint instSrv, uint assetSrv,
                   out VgVisSurface s)
{
    s = (VgVisSurface)0;
    if (vis == VG_VIS_EMPTY) return false;
    s.slot = vis >> VG_VIS_TRI_BITS;
    s.tri  = vis & VG_VIS_TRI_MASK;

    RWByteAddressBuffer work = ResourceDescriptorHeap[workUav];
    const uint2 e = work.Load2(visibleOff + s.slot * 8u);      // {instance, clusterRef}
    s.instance = e.x;
    s.clusterRef = e.y;
    StructuredBuffer<VgInstance> instances = ResourceDescriptorHeap[instSrv];
    StructuredBuffer<VgAssetGpu> assets     = ResourceDescriptorHeap[assetSrv];
    s.inst = instances[e.x];
    s.asset = assets[s.inst.assetIndex];

    const uint page = e.y & 0xFFFFu;
    const uint cidx = (e.y >> 16) & 0xFFu;
    const uint pageBase = (page & ((1u << VG_CHUNK_PAGES_LOG2) - 1u)) * VG_PAGE_BYTES;
    ByteAddressBuffer pool = ResourceDescriptorHeap[NonUniformResourceIndex(s.asset.poolSrvBase + (page >> VG_CHUNK_PAGES_LOG2))];
    const VgClusterRaster cl = VgLoadClusterRaster(pool, pageBase, cidx);
    s.materialIndex = cl.materialIndex;
    s.level = (cl.flags >> 8) & 0xFFu;

    const uint3 t = VgReadTri(pool, pageBase, cl, s.tri);
    s.lp0 = VgDecodePos(pool, pageBase, cl, t.x, s.asset.posOrigin, s.asset.posStep);
    s.lp1 = VgDecodePos(pool, pageBase, cl, t.y, s.asset.posOrigin, s.asset.posStep);
    s.lp2 = VgDecodePos(pool, pageBase, cl, t.z, s.asset.posOrigin, s.asset.posStep);
    s.w0 = VgXform(s.inst.w0, s.inst.w1, s.inst.w2, s.lp0);
    s.w1 = VgXform(s.inst.w0, s.inst.w1, s.inst.w2, s.lp1);
    s.w2 = VgXform(s.inst.w0, s.inst.w1, s.inst.w2, s.lp2);

    // 法線 / UV ストリーム（頂点ブロック = 位置 | 法線 u32[vc] | UV u32[vc]。各 16 整列）
    const uint bpp = cl.bits.x + cl.bits.y + cl.bits.z;
    const uint posBytes = (((cl.vertexCount * bpp + 7u) >> 3) + 15u) & ~15u;
    const uint nrmBase = pageBase + cl.vertexOffset + posBytes;
    const uint uvBase  = nrmBase + ((4u * cl.vertexCount + 15u) & ~15u);
    s.n0 = normalize(VgXformDir(s.inst, VgDecodeOct16(pool.Load(nrmBase + 4u * t.x))));
    s.n1 = normalize(VgXformDir(s.inst, VgDecodeOct16(pool.Load(nrmBase + 4u * t.y))));
    s.n2 = normalize(VgXformDir(s.inst, VgDecodeOct16(pool.Load(nrmBase + 4u * t.z))));
    const float4 uvh = asfloat(pool.Load4(pageBase + 64u + cidx * 128u + 96u));   // uvBase.xy, uvScale.xy
    const uint q0 = pool.Load(uvBase + 4u * t.x), q1 = pool.Load(uvBase + 4u * t.y), q2 = pool.Load(uvBase + 4u * t.z);
    s.t0 = uvh.xy + float2(q0 & 0xFFFFu, q0 >> 16) / 65535.0 * uvh.zw;
    s.t1 = uvh.xy + float2(q1 & 0xFFFFu, q1 >> 16) / 65535.0 * uvh.zw;
    s.t2 = uvh.xy + float2(q2 & 0xFFFFu, q2 >> 16) / 65535.0 * uvh.zw;

    // 重心座標（ラスタと同じジッタ付き VP のクリップ座標から）
    const float4 c0 = mul(float4(s.w0, 1.0), vpJ);
    const float4 c1 = mul(float4(s.w1, 1.0), vpJ);
    const float4 c2 = mul(float4(s.w2, 1.0), vpJ);
    VgBarycentrics(c0, c1, c2, svPos, vp, s.bary, s.baryDx, s.baryDy);

    const float3 b = s.bary;
    s.localPos    = s.lp0 * b.x + s.lp1 * b.y + s.lp2 * b.z;
    s.worldPos    = s.w0 * b.x + s.w1 * b.y + s.w2 * b.z;
    s.worldPosDx  = s.w0 * s.baryDx.x + s.w1 * s.baryDx.y + s.w2 * s.baryDx.z;
    s.worldPosDy  = s.w0 * s.baryDy.x + s.w1 * s.baryDy.y + s.w2 * s.baryDy.z;
    s.worldNormal = s.n0 * b.x + s.n1 * b.y + s.n2 * b.z;
    s.uv   = s.t0 * b.x + s.t1 * b.y + s.t2 * b.z;
    s.uvDx = s.t0 * s.baryDx.x + s.t1 * s.baryDx.y + s.t2 * s.baryDx.z;
    s.uvDy = s.t0 * s.baryDy.x + s.t1 * s.baryDy.y + s.t2 * s.baryDy.z;
    return true;
}

// 面がカメラへ向いているか（D3D の既定 = 画面上で時計回りが表。左手系ではワールドの cross(e1, e2) が視点側を向く）。
// 深度プリパスの SV_IsFrontFace と同じ判定（ミラーのインスタンスでも、変換後のワールド座標で測るので一致する）。
bool VgIsFrontFacing(VgVisSurface s, float3 camPos)
{
    return dot(cross(s.w1 - s.w0, s.w2 - s.w0), camPos - s.w0) > 0.0;
}

// 材質（未登録 / 範囲外は既定: 白・metallic 0・roughness 0.5）
VgMaterialGpu VgLoadMaterial(uint materialsSrv, VgAssetGpu asset, uint mi)
{
    VgMaterialGpu m = (VgMaterialGpu)0;
    m.albedoSrv = VG_NONE; m.normalSrv = VG_NONE; m.metalRoughSrv = VG_NONE; m.emissiveSrv = VG_NONE;
    m.flags = 0u;
    m.metallic = 0.0;
    m.roughness = 0.5;
    m.uvScaleOffset = float4(1.0, 1.0, 0.0, 0.0);
    if (asset.materialBase != VG_NONE && mi < asset.materialCount)
    {
        StructuredBuffer<VgMaterialGpu> mats = ResourceDescriptorHeap[materialsSrv];
        m = mats[asset.materialBase + mi];
    }
    return m;
}

// 実効の metallic / roughness（MeshRenderer の上書き > 材質。ApplicationRender.cpp の pbrParams と同じ優先度）
float VgEffMetallic(VgMaterialGpu m, VgInstance inst)  { return inst.overrideMetallic  >= 0.0 ? inst.overrideMetallic  : m.metallic; }
float VgEffRoughness(VgMaterialGpu m, VgInstance inst) { return inst.overrideRoughness >= 0.0 ? inst.overrideRoughness : m.roughness; }

// 自己発光（テクスチャを掛ける前）。Material.h の ResolveEmissiveParams → PackEmissive → Forward.hlsl の UnpackEmissive と
// 同じ値（8bit 量子化まで同じ）になるように、上書きを合成してから量子化する。
float3 VgResolveEmissive(VgMaterialGpu m, VgInstance inst)
{
    float3 col = ((inst.flags & VG_INST_EMIS_COLOR_OV) != 0u)
        ? float3((inst.packedEmissive >> 16) & 0xFFu, (inst.packedEmissive >> 8) & 0xFFu, inst.packedEmissive & 0xFFu) / 255.0
        : m.emissiveColor;
    float I = m.emissiveIntensity;
    if ((inst.flags & VG_INST_EMIS_INT_OV) != 0u)
    {
        const float n = float((inst.packedEmissive >> 24) & 0xFFu) / 255.0;
        I = n * n * 64.0;
    }
    if (I > 0.0 && col.x <= 0.0 && col.y <= 0.0 && col.z <= 0.0) col = float3(1.0, 1.0, 1.0);
    I = clamp(I, 0.0, 64.0);
    if (I <= 0.0) return float3(0.0, 0.0, 0.0);
    const float3 cq = floor(saturate(col) * 255.0 + 0.5);                 // QuantizeUnorm8
    const float  nq = floor(saturate(sqrt(I / 64.0)) * 255.0 + 0.5) / 255.0;
    return cq / 255.0 * (nq * nq * 64.0);
}

// 接線（頂点に持たないので三角形の UV 勾配から作る）。assimp の CalcTangentSpace と同じ式・同じ符号規約:
//   tangent = (dp1 * du2.y − dp2 * du1.y) * sign(r) / bitangent(assimp) = −(dp2 * du1.x − dp1 * du2.x) * sign(r)
//   （r = du1.x * du2.y − du2.x * du1.y。assimp は FlipUVs の後に計算するので、エンジンの UV 規約では数学上の B と逆向き）
//   tangentW = ModelLoader.cpp と同じ: dot(cross(N, T), bitangent) < 0 なら −1。
void VgTangentFrame(VgVisSurface s, float4 uvso, float3 N, out float3 T, out float tangentW)
{
    const float3 dp1 = s.w1 - s.w0, dp2 = s.w2 - s.w0;
    const float2 du1 = (s.t1 - s.t0) * uvso.xy, du2 = (s.t2 - s.t0) * uvso.xy;
    const float r = du1.x * du2.y - du2.x * du1.y;
    const float sg = r < 0.0 ? -1.0 : 1.0;
    float3 t = (dp1 * du2.y - dp2 * du1.y) * sg;
    float3 bAssimp = -(dp2 * du1.x - dp1 * du2.x) * sg;
    if (!(dot(t, t) > 1e-30))
    {
        // UV が退化: 法線に直交する適当な軸
        t = abs(N.y) < 0.99 ? cross(float3(0, 1, 0), N) : cross(float3(1, 0, 0), N);
        bAssimp = cross(N, t);
    }
    T = normalize(t);
    tangentW = (dot(cross(N, T), bAssimp) < 0.0) ? -1.0 : 1.0;
}

#endif // VG_VIS_SHADE_HLSLI
