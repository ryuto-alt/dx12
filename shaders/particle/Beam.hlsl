// Beam.hlsl - 連続ビーム / 火柱 / 稲妻（2点間カメラ向きquad）
//   離散ビルボードの寄せ集めをやめ、A→B を1枚のカメラ向きクアッドで繋ぐ。
//   side = cross(beamDir, toCamera) で軸ビルボード化。PS で kind ごとに数式シェーディング。
//   energy=コア+ハロー+スクロール、electric=fbm変位コア+ストロボ、fire=縦スクロール火柱。
//   HDR 加算 → ブルームで白熱。シーン深度 SRV で soft particle フェード。
#include "ParticleCommon.hlsli"

cbuffer BeamCB : register(b0)
{
    float4x4 viewProj;   // 行ベクトル前提
    float4   camPos;     // ワールドカメラ位置 (xyz)
    float4   params;     // x=globalIntensity, y=time, z=softFadeDist, w=未使用
    float4   params2;    // x=projA(_33), y=projB(_43), z=1/RTw, w=1/RTh (z<=0 で soft 無効)
    float4   pad0;       // 32 DWORD に揃える
};

Texture2D    gSceneDepth : register(t0);
SamplerState sDepth      : register(s0);

#define BEAM_ENERGY   0
#define BEAM_ELECTRIC 1
#define BEAM_FIRE     2

struct VSInput
{
    float3 p0      : POSITION;     // 始点
    float3 p1      : NORMAL;       // 終点
    float4 color   : COLOR0;       // rgb=HDR色, a=アルファ
    float  halfW   : TEXCOORD0;    // 半幅
    float  age01   : TEXCOORD1;
    uint   kind    : TEXCOORD2;
    float  seed    : TEXCOORD3;
    uint   vid     : SV_VertexID;
};

struct VSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    float2 uv    : TEXCOORD0;    // u=長手 0..1, v=幅 -1..1
    float  age01 : TEXCOORD1;
    nointerpolation uint kind : TEXCOORD2;
    float  seed  : TEXCOORD3;
    float  viewZ : TEXCOORD4;
};

// 四角形2三角形分の (end, side)
static const float2 kBeamCorners[6] = {
    float2(0, -1), float2(1, -1), float2(1, 1),
    float2(0, -1), float2(1,  1), float2(0, 1)
};

VSOutput VSMain(VSInput i)
{
    float2 cc = kBeamCorners[i.vid];
    float3 P  = lerp(i.p0, i.p1, cc.x);
    float3 dir = i.p1 - i.p0;
    float len = length(dir);
    dir = (len > 1e-5) ? dir / len : float3(0, 1, 0);
    float3 toCam = normalize(camPos.xyz - P);
    float3 sideV = cross(dir, toCam);
    if (dot(sideV, sideV) < 1e-6) sideV = cross(dir, float3(0, 1, 0));  // 軸方向視の退化ガード
    sideV = normalize(sideV);
    float3 worldPos = P + sideV * (i.halfW * cc.y);

    VSOutput o;
    o.pos   = mul(float4(worldPos, 1.0), viewProj);
    o.color = i.color;
    o.uv    = float2(cc.x, cc.y);
    o.age01 = i.age01;
    o.kind  = i.kind;
    o.seed  = i.seed;
    o.viewZ = o.pos.w;
    return o;
}

float4 PSMain(VSOutput i) : SV_TARGET
{
    // soft particles を先に評価して隠れた画素を捨てる（火柱は遮蔽フェードしない）。
    // 高コストな fbm シェーディングの前に clip する。
    float soft = 1.0;
    if (params2.z > 0.0 && i.kind != BEAM_FIRE)
    {
        float2 suv = i.pos.xy * float2(params2.z, params2.w);
        float sceneD = gSceneDepth.SampleLevel(sDepth, suv, 0).r;
        float sceneZ = params2.y / (sceneD - params2.x);
        soft = saturate((sceneZ - i.viewZ) / max(params.z, 1e-3));
    }
    float a = i.color.a * soft;
    clip(a - 1e-3);

    float u = i.uv.x;            // 長手
    float v = i.uv.y;            // 幅 [-1,1]
    float av = abs(v);
    float t = params.y;
    float3 col;

    if (i.kind == BEAM_FIRE)
    {
        // 動的火柱: age01 で噴き上がり→うねり持続→崩れ。fbm炎を縦スクロール＋温度ランプ。
        float rise  = smoothstep(0.0, 0.28, i.age01);          // 噴き上がり
        float fall  = 1.0 - smoothstep(0.62, 1.0, i.age01);    // 崩れ
        float reach = saturate(rise * 1.15);                   // 現在の到達高さ(0..1)
        float2 p = float2(v * 2.0, u * 3.2 - t * 2.6 + i.seed * 10.0);
        float n = fbmWarp(p * 1.2, 3);
        float flick = 0.7 + 0.4 * fbm2(float2(t * 6.0 + i.seed, u * 2.0), 2);
        float taper = saturate(1.0 - u * 0.8);
        float width = lerp(1.0, 0.25, u) * (0.78 + 0.44 * (0.5 + 0.5 * n)); // うねる幅
        float body = saturate(1.0 - av / max(width, 1e-3));
        float hmask = saturate((reach - u) * 6.0 + 0.5);       // reach まで炎が満ちる
        float flame = saturate(body * (0.45 + 0.9 * (0.5 + 0.5 * n)) * (0.35 + taper)) * hmask * flick * fall;
        flame = pow(flame, 1.4);
        float temp = saturate(flame * 1.4);
        float3 hot = float3(1.0, 0.92, 0.72);
        col = lerp(i.color.rgb, hot, saturate(temp - 0.4) * 1.4) * (0.5 + flame);
        col *= flame;
    }
    else if (i.kind == BEAM_ELECTRIC)
    {
        // 稲妻: 時間離散化したシード→fbm でコアを変位（ストロボ）
        float boltSeed = floor(t * 15.0 + i.seed * 13.0);
        float centerline = (fbm2(float2(u * 8.0 + boltSeed * 7.0, i.seed), 4)) * 0.55;
        float d = abs(v - centerline);
        float glow = 0.02 / (d + 0.012) + exp(-d * d * 220.0) * 2.2;
        float flick = 0.55 + 0.45 * frac(sin(boltSeed * 78.233) * 43758.5453);
        col = i.color.rgb * glow * flick;
        col *= smoothstep(0.0, 0.05, u) * smoothstep(1.0, 0.95, u);
    }
    else // BEAM_ENERGY
    {
        float core = exp(-av * av * 90.0);                    // 細い白熱コア
        float halo = exp(-av * av * 5.0);                     // 広いハロー
        col = i.color.rgb * (core * 1.6 + halo * 0.6);
        col += core * core * 2.0;                             // 白熱の芯（ブルーム源）
        col *= lerp(0.75, 1.3, fbm2_01(float2(u * 22.0 + t * 9.0, i.seed), 3)); // プラズマ流れ
        col *= smoothstep(0.0, 0.035, u) * smoothstep(1.0, 0.94, u);            // 端テーパ
    }

    return float4(col * a * params.x, a);
}
