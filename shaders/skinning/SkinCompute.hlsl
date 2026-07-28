// SkinCompute.hlsl — compute スキニング（計画09 Step 4）
//
// 目的: スキンドメッシュの「変形後の頂点位置」を GPU バッファへ書き出し、DXR の BLAS を
//       建てられるようにする。これが無いとスキンドは TLAS に入れられず（変形後の頂点が
//       GPU のどこにも存在しないため）、キャラだけ RT 影 / RT-AO の対象外になる。
//
// ★スキニング式は shaders/forward/ForwardSkinned.hlsl:85-91 と**1文字も変えていない**。
//   ここがズレると「RT の影」と「ラスタの絵」が食い違う（キャラの足元だけ影がずれる等）。
//   特に:
//     - Σ(weight * bone) で skinMatrix を作ってから mul する順序（ShadowPassSkinned.hlsl の
//       Σ(weight * mul(pos, bone)) 順は数学的には等価でも浮動小数の結果が違うので採らない）
//     - totalWeight==0 のフォールバックは**無し**（skinMatrix=0 → 原点に潰れる）。
//       ForwardSkinned がそうなっているので合わせる。
//     - g_bones は転置済み（AnimPose.cpp が XMMatrixTranspose して置く）＝ mul(row, mat) 規約。
//       型宣言を ForwardSkinned.hlsl:11 と同一に保つこと（row_major 等を足さない）。
//
// 出力はオブジェクトローカル空間。ForwardSkinned.hlsl:96 が skinnedPos に model を
// 掛けてワールドへ運んでいるので、ワールド変換は TLAS のインスタンス Transform に任せる
// ＝静的メッシュとまったく同じ形になる。
//
// ponytail: 出力は位置のみ 12B/頂点。RT 影 / RT-AO はヒット時に何も読まない
//   （RtCommon.hlsli の RayQuery は CommittedStatus / CommittedRayT しか見ない）ので
//   法線もUVも要らない。DDGI（Step 7）でヒットシェーディングを始めるときに法線を足すこと。

cbuffer SkinCB : register(b0)
{
    uint gVertexCount;    // このメッシュの頂点数
    uint gSrcStride;      // 入力頂点のストライド（= sizeof(Vertex) = 96）
    uint gPad0;
    uint gPad1;
};

// 入力頂点バッファ。Mesh.h の Vertex そのままのインターリーブ配置:
//   position 0 / normal 12 / color 24 / texCoord 40 / tangent 48 / boneIndices 64 / boneWeights 80
ByteAddressBuffer          g_vertices : register(t0);
StructuredBuffer<float4x4> g_bones    : register(t1);

// 変形後の位置だけを詰めた出力（float3 × 頂点数）。BLAS の VertexBuffer にそのまま渡す。
RWByteAddressBuffer        g_out      : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint vid = dtid.x;
    if (vid >= gVertexCount)
        return;

    const uint base = vid * gSrcStride;

    const float3 position    = asfloat(g_vertices.Load3(base +  0));
    const uint4  boneIndices = g_vertices.Load4(base + 64);
    const float4 boneWeights = asfloat(g_vertices.Load4(base + 80));

    // ---- ここから ForwardSkinned.hlsl:85-91 と同一 ----
    float4x4 skinMatrix =
        boneWeights.x * g_bones[boneIndices.x] +
        boneWeights.y * g_bones[boneIndices.y] +
        boneWeights.z * g_bones[boneIndices.z] +
        boneWeights.w * g_bones[boneIndices.w];

    float4 skinnedPos = mul(float4(position, 1.0f), skinMatrix);
    // ---- ここまで ----

    g_out.Store3(vid * 12, asuint(skinnedPos.xyz));
}
