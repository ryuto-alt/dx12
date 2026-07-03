// GpuParticleDraw.hlsl - GPUパーティクルの描画 VS。
// 頂点バッファ無し: SV_VertexID で四角形、SV_InstanceID で alive リスト → 粒子を引く。
// 出力シグネチャは Particle.hlsl の VSOutput と完全一致させ、PS は Particle_PS.cso
// （プロシージャル kind 質感 + soft particles）をそのまま流用する。
// ルートシグネチャ: b0=CamCB(32定数,ALL) + t0=深度(PS,table) + t1/t2=粒子/alive(ルートSRV,VS) + s0

cbuffer CamCB : register(b0)
{
    float4x4 viewProj;   // 転置済み（mul(rowvec, M)）
    float4   camRight;
    float4   camUp;
    float4   params;     // x=globalIntensity, y=glowSoftness, z=time, w=softFadeDist
    float4   params2;    // x=projA, y=projB, z=1/RTw, w=1/RTh
};

// 粒子 96B（GpuParticleSim.hlsl と一致必須）
struct GPart
{
    float3 pos;   float life;
    float3 vel;   float age;
    float3 col0;  float size0;
    float3 col1;  float size1;
    float gravity; float drag; float turb; float seed;
    uint  kind;   float stretch; float2 _pad;
};

StructuredBuffer<GPart> gParticles : register(t1);
StructuredBuffer<uint>  gAlive     : register(t2);

// Particle.hlsl の VSOutput と完全一致（PS 流用のため）。GPU パーティクルはテクスチャ貼り付け
// 非対応なので texIdx は常に kNoTexture(0xFFFFFFFF) を出力しプロシージャル質感に固定する。
struct VSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    float2 uv    : TEXCOORD0;
    float  age01 : TEXCOORD1;
    nointerpolation uint kind : TEXCOORD2;
    float  seed  : TEXCOORD3;
    float  viewZ : TEXCOORD4;
    nointerpolation uint texIdx : TEXCOORD5;
};

static const float2 kCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1),
    float2(-1, -1), float2(1,  1), float2(-1, 1)
};

VSOutput VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    GPart p = gParticles[gAlive[iid]];
    float2 c = kCorners[vid];

    float t    = saturate(p.age / p.life);
    float fade = (1.0 - t) * (1.0 - t);
    float size = p.size0 + (p.size1 - p.size0) * t;

    float3 worldPos;
    if (p.stretch > 0.0 && dot(p.vel, p.vel) > 1e-4)
    {
        float3 vd = normalize(p.vel);
        float3 axisX = cross(vd, camRight.xyz);
        if (dot(axisX, axisX) < 1e-5) axisX = camUp.xyz;
        axisX = normalize(axisX);
        float speedMag = length(p.vel);
        float halfW = size * 0.55;
        float halfH = size * (1.0 + p.stretch * min(speedMag, 30.0) * 0.06);
        worldPos = p.pos + axisX * (c.x * halfW) + vd * (c.y * halfH);
    }
    else
    {
        worldPos = p.pos + camRight.xyz * (c.x * size) + camUp.xyz * (c.y * size);
    }

    VSOutput o;
    o.pos   = mul(float4(worldPos, 1.0), viewProj);
    o.color = float4(lerp(p.col0, p.col1, t), fade);
    o.uv    = c;
    o.age01 = t;
    o.kind  = p.kind;
    o.seed  = p.seed;
    o.viewZ = o.pos.w;
    o.texIdx = 0xFFFFFFFFu;   // kNoTexture: 常にプロシージャル
    return o;
}
