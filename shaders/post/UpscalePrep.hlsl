// UpscalePrep.hlsl - 時間方向アップスケーラ（DLSS）へ渡すモーションベクトルを作る compute。
//
// 速度プリパスは「描いた物」の画素にしか速度を書かない（RT のクリア値は 0）。空・スカイボックス
// （深度 = 1 のまま）は 0 のままなので、そのまま DLSS へ渡すとカメラを回したとき空が引きずられる。
// エンジンの TAA は解決シェーダの中で空の速度を深度から復元している（TAA.hlsl）。DLSS は外部の
// ブラックボックスなので、同じ復元をここで済ませた MV を別テクスチャへ書いて渡す。
//
// 単位と符号: 入力の速度バッファと同じ「レンダー解像度のビューポート UV、現UV - 前UV、ジッタなし」。
//             DLSS には InMVScale = (-renderW, -renderH) を渡してピクセル単位の「前 - 現」へ直させる。
//
// 規約: 深度は標準 Z（0 = near / 1 = far。HiZMath.h の根拠を参照）。

cbuffer PrepCB : register(b0)
{
    float4x4 invViewProj;    // 現（ジッタなし）viewProj の逆行列（転置済み）
    float4x4 prevViewProj;   // 前フレーム（ジッタなし）viewProj（転置済み）
    uint2    size;           // レンダー解像度
    uint2    _pad;
};

Texture2D<float>   gDepth    : register(t0);
Texture2D<float2>  gVelocity : register(t1);
RWTexture2D<float2> gOut     : register(u0);

[numthreads(8, 8, 1)]
void UpscalePrepCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;

    const float depth = gDepth.Load(int3(id.xy, 0));
    float2 vel;
    if (depth >= 1.0 - 1e-6)
    {
        // 空 / 未描画: カメラ運動だけで再投影する（TAA.hlsl / MotionBlur.hlsl と同じ式）。
        const float2 uv  = (float2(id.xy) + 0.5) / float2(size);
        const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        float4 world = mul(invViewProj, float4(ndc, depth, 1.0));
        world /= (abs(world.w) > 1e-6 ? world.w : 1e-6);
        const float4 pc     = mul(prevViewProj, world);
        const float2 prevNd = pc.xy / max(abs(pc.w), 1e-6);
        const float2 prevUv = float2(prevNd.x * 0.5 + 0.5, 0.5 - prevNd.y * 0.5);
        vel = uv - prevUv;
    }
    else
    {
        vel = gVelocity.Load(int3(id.xy, 0));
    }
    gOut[id.xy] = vel;
}
