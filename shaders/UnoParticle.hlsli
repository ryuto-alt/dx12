// ============================================================================
// UnoParticle.hlsli — カスタムパーティクルシェーダーの共通土台
//
//   assets/shaders/*.hlsl（ParticleLayer::shaderPath に割り当てる自作シェーダー）から
//     #include "UnoParticle.hlsli"
//   と書くだけで、定数・入出力・四角形の展開・ソフトパーティクルが揃う。
//
// ■ メッシュ用（UnoCustom.hlsli）とは別契約
//   b0 の中身も register も全く違う。メッシュ用の雛形をそのまま貼っても動かない。
//   粒子が使えるのは b0 / t0=シーン深度 / t2=テクスチャ / s0 だけ。
//   これ以外の register を宣言すると、コンパイルは通っても PSO 生成で落ちる。
//
// ■ 最小形
//     #include "UnoParticle.hlsli"
//     VSOutput VSMain(VSInput i) { return UnoParticleVS(i); }
//     float4   PSMain(VSOutput i) : SV_TARGET
//     {
//         float a = UnoSoftRound(i.uv, 0.6) * UnoSoftParticle(i);
//         return float4(i.color.rgb * a, i.color.a * a);
//     }
//
// ■ 出力は「前乗算アルファ」
//   加算ブレンドは (色, α) を Src=ONE で足す。α ブレンドは Dest=INV_SRC_ALPHA。
//   どちらも色はあらかじめ α を掛けた値を返すこと。掛け忘れると加算では
//   ふちが四角く光り、α では二重に濃くなる。
// ============================================================================
#ifndef UNO_PARTICLE_HLSLI
#define UNO_PARTICLE_HLSLI

#include "particle/ParticleCommon.hlsli"   // ノイズ/fbm/パレット/SDF（テクスチャ不要の質感づくり）

cbuffer CamCB : register(b0)
{
    float4x4 viewProj;   // 行ベクトル前提（mul(rowvec, M)）。CPU 側で転置済み
    float4   camRight;   // ワールド空間のカメラ右ベクトル (xyz)
    float4   camUp;      // ワールド空間のカメラ上ベクトル (xyz)
    float4   params;     // x=全体強度 y=グロー柔らかさ z=時間(秒) w=ソフトフェード距離
    float4   params2;    // x=projA(_33) y=projB(_43) z=1/RT幅 w=1/RT高（z<=0 でソフト無効）
};

Texture2D    gSceneDepth : register(t0);   // R32_FLOAT シーン深度（NDC z）
Texture2D    gAlbedo     : register(t2);   // 粒子テクスチャ（texIdx != kNoTexture のときだけ）
SamplerState sDepth      : register(s0);   // LINEAR CLAMP（深度/テクスチャ共用）

#define kNoTexture 0xFFFFFFFFu

struct VSInput
{
    float3 center  : POSITION;    // ワールド中心
    float  size    : TEXCOORD0;   // 半径（ワールド）
    float4 color   : COLOR0;      // rgb=HDR色, a=アルファ
    float  rot     : TEXCOORD1;   // 回転（ラジアン）
    float  stretch : TEXCOORD2;   // >0 で速度方向へ伸ばす
    float3 vel     : NORMAL0;     // ワールド速度
    float  age01   : TEXCOORD3;   // 寿命 0..1（0=生まれた瞬間, 1=消える）
    uint   kind    : TEXCOORD4;   // 下位16bit=見た目種別 上位16bit=向きモード
    float  seed    : TEXCOORD5;   // 個体差のシード
    uint   texIdx  : TEXCOORD6;   // テクスチャの SRV 添字（kNoTexture=無し）
    uint   vid     : SV_VertexID;
};

struct VSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    float2 uv    : TEXCOORD0;    // [-1,1] 中心原点
    float  age01 : TEXCOORD1;
    nointerpolation uint kind : TEXCOORD2;
    float  seed  : TEXCOORD3;
    float  viewZ : TEXCOORD4;    // ビュー空間 Z（= clip.w）
    nointerpolation uint texIdx : TEXCOORD5;
};

static const float2 kUnoCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1),
    float2(-1, -1), float2(1,  1), float2(-1, 1)
};

// 標準の頂点シェーダー。ビルボード展開・向きモード・速度ストレッチを全部やる。
// 粒子の位置を動かしたいだけなら center を触ってから呼べばよい。
VSOutput UnoParticleVS(VSInput i)
{
    float2 c = kUnoCorners[i.vid];
    float3 worldPos;

    const uint orient = i.kind >> 16;
    const uint kind   = i.kind & 0xFFFFu;

    if (i.stretch > 0.0 && dot(i.vel, i.vel) > 1e-4)
    {
        // 速度方向へ伸びるビルボード（火花・筋・弾道）。向きモードより優先。
        float3 dir  = normalize(i.vel);
        float3 side = normalize(cross(dir, camRight.xyz - dir * dot(dir, camRight.xyz)) + 1e-6);
        side = normalize(cross(dir, normalize(cross(camRight.xyz, dir) + 1e-6)));
        worldPos = i.center + dir * (c.y * i.size * (1.0 + i.stretch)) + side * (c.x * i.size);
    }
    else
    {
        float s = sin(i.rot), co = cos(i.rot);
        float2 r = float2(c.x * co - c.y * s, c.x * s + c.y * co) * i.size;
        if (orient == 1)       worldPos = i.center + float3(r.x, 0, r.y);          // 水平（地面向き）
        else if (orient == 2)  worldPos = i.center + float3(r.x, r.y, 0);          // 垂直（+Z 正対）
        else                   worldPos = i.center + camRight.xyz * r.x + camUp.xyz * r.y; // カメラ正対
    }

    VSOutput o;
    float4 clip = mul(float4(worldPos, 1.0), viewProj);
    o.pos   = clip;
    o.color = i.color;
    o.uv    = c;
    o.age01 = i.age01;
    o.kind  = kind;
    o.seed  = i.seed;
    o.viewZ = clip.w;
    o.texIdx = i.texIdx;
    return o;
}

// 時間（秒）。params.z の別名。
float UnoTime() { return params.z; }

// 丸いソフトな減衰。edge を小さくすると芯が硬く、大きくするとぼんやりする。
float UnoSoftRound(float2 uv, float edge)
{
    return saturate(1.0 - length(uv)) * saturate(1.0 - length(uv) * edge);
}

// ソフトパーティクル。シーンの手前に何かあれば消し、接地面との交差線を柔らげる。
// 戻り値 0..1 を最終アルファに掛ける。params2.z <= 0 のときは常に 1（無効）。
//
// ★これを掛けないと、床や壁に粒子が【紙のように刺さって】見える。
//   手動オクルージョンも兼ねているので、掛けないと壁の向こうの粒子まで見える。
float UnoSoftParticle(VSOutput i)
{
    if (params2.z <= 0.0) return 1.0;
    float2 suv = i.pos.xy * float2(params2.z, params2.w);
    float  sceneZ = gSceneDepth.SampleLevel(sDepth, suv, 0).r;
    if (sceneZ >= 1.0) return 1.0;                       // 何も描かれていない（空）
    float sceneView = params2.y / max(sceneZ - params2.x, 1e-6);   // NDC z → ビュー空間 Z
    float diff = sceneView - i.viewZ;
    if (diff < 0.0) return 0.0;                          // 粒子の手前に物がある＝隠れる
    return saturate(diff / max(params.w, 1e-4));
}

// 粒子テクスチャ。割り当てが無ければ白を返す（呼び出し側は常にこれを掛けてよい）。
float4 UnoParticleTex(VSOutput i)
{
    if (i.texIdx == kNoTexture) return float4(1, 1, 1, 1);
    return gAlbedo.SampleLevel(sDepth, i.uv * 0.5 + 0.5, 0);
}

#endif // UNO_PARTICLE_HLSLI
