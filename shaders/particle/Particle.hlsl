// Particle.hlsl - GPUインスタンシング ビルボードパーティクル（プロシージャル質感）
//   頂点バッファ無し: SV_VertexID で四角形を生成、インスタンスストリームで粒子情報を受ける。
//   見た目はテクスチャ不要 ── PS で kind ごとに数式で形状/質感を生成する（火/煙/火花/魔法/電撃/リング/スター）。
//   HDR(RGBA16F) へ加算 or 前乗算アルファで描き、ポスト側のブルーム＋トーンマップで白熱して光る。
//   シーン深度 SRV をサンプルして「手動オクルージョン＋接地ソフトフェード」を行う（soft particles）。
#include "ParticleCommon.hlsli"

cbuffer CamCB : register(b0)
{
    float4x4 viewProj;   // 行ベクトル前提 (mul(rowvec, M))。CPU側で転置して渡す
    float4   camRight;   // ワールド空間カメラ右ベクトル (xyz)
    float4   camUp;      // ワールド空間カメラ上ベクトル (xyz)
    float4   params;     // x=globalIntensity, y=glowSoftness, z=time, w=softFadeDist
    float4   params2;    // x=projA(_33), y=projB(_43), z=1/RTw, w=1/RTh  （z<=0 で soft 無効）
};

Texture2D    gSceneDepth : register(t0);   // R32_FLOAT シーン深度（NDC z）
// t1 は GpuParticleDraw.hlsl が StructuredBuffer<GPart>(VS,root SRV) として使用中のため、
// このPS専用テクスチャは t2 に置く（同じ Particle_PS.cso を GPU パーティクル描画でも共用するため）。
Texture2D    gAlbedo     : register(t2);   // 粒子アルベド（texIndex!=0xFFFFFFFF の時のみサンプル）
SamplerState sDepth      : register(s0);   // LINEAR CLAMP（デプス/アルベド共用）

#define kNoTexture 0xFFFFFFFFu

// kind 定義（C++ 側 ParticleKind と一致必須）
#define KIND_GLOW     0
#define KIND_FIRE     1
#define KIND_SMOKE    2
#define KIND_SPARK    3
#define KIND_MAGIC    4
#define KIND_ELECTRIC 5
#define KIND_RING     6
#define KIND_STAR     7

struct VSInput
{
    float3 center  : POSITION;    // ワールド中心
    float  size    : TEXCOORD0;   // 半径(ワールド)
    float4 color   : COLOR0;      // rgb=HDR色, a=アルファ
    float  rot     : TEXCOORD1;   // 回転(ラジアン)
    float  stretch : TEXCOORD2;   // >0 で速度方向へ伸ばす
    float3 vel     : NORMAL0;     // ワールド速度（ストレッチ用）
    float  age01   : TEXCOORD3;   // 寿命 0..1
    uint   kind    : TEXCOORD4;   // 見た目の種別
    float  seed    : TEXCOORD5;   // 個体差用シード
    uint   texIdx  : TEXCOORD6;   // SRVヒープの絶対インデックス。kNoTexture ならプロシージャル
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

// 四角形2三角形分のコーナー
static const float2 kCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1),
    float2(-1, -1), float2(1,  1), float2(-1, 1)
};

VSOutput VSMain(VSInput i)
{
    float2 c = kCorners[i.vid];
    float3 worldPos;

    // kind の上位ビットに粒子の向きモードがパックされている（C++側 ParticleSystem の GpuParticle 詰め参照）。
    // 0=ビルボード（従来） 1=水平（XZ地面向き。リング/衝撃波/魔法陣） 2=垂直（XY,+Z正対。壁エフェクト）
    uint orient = i.kind >> 16;
    uint kind   = i.kind & 0xFFFFu;

    if (i.stretch > 0.0 && dot(i.vel, i.vel) > 1e-4)
    {
        // 速度方向に伸びるストレッチビルボード（火花/筋/弾道）。orient は無視（速度整列が優先）。
        float3 vd = normalize(i.vel);
        float3 axisX = cross(vd, camRight.xyz);
        if (dot(axisX, axisX) < 1e-5) axisX = camUp.xyz;
        axisX = normalize(axisX);
        float speedMag = length(i.vel);
        float halfW = i.size * 0.55;
        float halfH = i.size * (1.0 + i.stretch * min(speedMag, 30.0) * 0.06);
        worldPos = i.center + axisX * (c.x * halfW) + vd * (c.y * halfH);
    }
    else
    {
        // 回転（ビルボードでは火花の向き / 水平・垂直では面内回転）
        float s = sin(i.rot), co = cos(i.rot);
        float2 r = float2(c.x * co - c.y * s, c.x * s + c.y * co);
        float3 axisX = camRight.xyz;
        float3 axisY = camUp.xyz;
        if (orient == 1u)        // 水平: XZ平面（法線+Y）
        {
            axisX = float3(1, 0, 0);
            axisY = float3(0, 0, 1);
        }
        else if (orient == 2u)   // 垂直: XY平面（法線+Z）
        {
            axisX = float3(1, 0, 0);
            axisY = float3(0, 1, 0);
        }
        worldPos = i.center
                 + axisX * (r.x * i.size)
                 + axisY * (r.y * i.size);
    }

    VSOutput o;
    o.pos   = mul(float4(worldPos, 1.0), viewProj);
    o.color = i.color;
    o.uv    = c;   // フォールオフは放射状なので未回転の c を使う
    o.age01 = i.age01;
    o.kind  = kind;   // PS には orient を剥がした純粋な kind を渡す
    o.seed  = i.seed;
    o.texIdx = i.texIdx;
    o.viewZ = o.pos.w;   // 標準射影では w = ビュー空間 Z
    return o;
}

// kind ごとの形状/質感。shape=0..1 の被覆、kindColor=HDR色 を返す。
void ShadeKind(VSOutput i, out float shape, out float3 kindColor)
{
    float2 uv = i.uv;
    kindColor = i.color.rgb;
    shape = 0.0;

    if (i.kind == KIND_FIRE)
    {
        float2 p = uv * float2(1.6, 1.3);
        float n = fbmWarp(p * 1.4 + float2(i.seed * 13.0, -i.age01 * 3.2 - i.seed * 7.0), 3);
        float body  = saturate(1.0 - length(uv * float2(1.0, 0.8)));
        float flame = saturate(body * (0.55 + 0.8 * (0.5 + 0.5 * n)) - (uv.y * 0.5 + 0.5) * 0.55 * i.age01);
        flame = pow(flame, 1.6);
        shape = flame;
        float temp = saturate(flame * 1.35);
        float3 hot = float3(1.0, 0.92, 0.72);   // 白熱コア
        kindColor = lerp(i.color.rgb, hot, saturate(temp - 0.45) * 1.3) * (0.55 + 0.95 * flame);
    }
    else if (i.kind == KIND_SMOKE)
    {
        float2 p = uv * 1.25;
        float d = fbm2_01(p * 1.8 + float2(i.seed * 9.0, -i.age01 * 1.1), 4);
        float mask = saturate(1.0 - length(uv));      // 四角を丸く
        float density = saturate((d - 0.34) * 1.9) * mask;
        density *= (1.0 - i.age01 * 0.15);
        shape = density;
        kindColor = i.color.rgb;                       // ゲーム指定色（暗めで遮蔽）
    }
    else if (i.kind == KIND_SPARK)
    {
        float r2 = dot(uv, uv);
        shape = 1.0 / (1.0 + 28.0 * r2);               // エネルギー保存の明るいコア
    }
    else if (i.kind == KIND_MAGIC)
    {
        float r = length(uv);
        float a = atan2(uv.y, uv.x);
        float swirl = fbm2(float2(a * 2.0 + i.age01 * 4.0 + i.seed * 10.0, r * 3.0 - i.age01 * 2.0), 3);
        float ring  = saturate(1.0 - abs(r - 0.55) / 0.4);
        float body  = saturate((0.5 + 0.5 * swirl) * (1.0 - r) + ring * 0.6);
        shape = pow(body, 1.5);
        float3 mc = palette(i.age01 + 0.3 * swirl, float3(0.5, 0.4, 0.7), float3(0.4, 0.3, 0.4),
                            float3(1, 1, 1), float3(0.0, 0.2, 0.45));
        kindColor = lerp(i.color.rgb, i.color.rgb * mc * 2.2, 0.6);
    }
    else if (i.kind == KIND_ELECTRIC)
    {
        float t = floor(params.z * 28.0 + i.seed * 97.0);          // 時間離散化＝ストロボ
        float jit = fbm2(float2(uv.y * 3.0 + t, i.seed * 20.0), 3) * 0.55;
        float d = abs(uv.x - jit * (1.0 - abs(uv.y)));             // Y沿いに走り X方向にジッタ
        float bolt = saturate(0.05 / (d + 0.02));
        float glow = saturate(0.22 / (dot(uv, uv) * 4.0 + 0.25));
        shape = saturate(bolt + glow * 0.5);
    }
    else if (i.kind == KIND_RING)
    {
        // 単一粒子で「拡大する衝撃波リング」を表現（age01 で半径が広がる）
        float r = length(uv);
        float rad = i.age01;
        float band = 0.12 * (1.0 - 0.6 * i.age01);
        float ring = saturate(1.0 - abs(r - rad) / max(band, 1e-3));
        ring *= saturate(1.0 - i.age01);
        shape = pow(ring, 1.4);
    }
    else if (i.kind == KIND_STAR)
    {
        float r = length(uv);
        float a = atan2(uv.y, uv.x);
        float streakH = exp(-abs(uv.y) * 14.0) * exp(-abs(uv.x) * 1.4);
        float streakV = exp(-abs(uv.x) * 14.0) * exp(-abs(uv.y) * 1.4);
        float core = 1.0 / (1.0 + 40.0 * r * r);
        float petals = pow(abs(cos(a * 3.0)), 6.0) * saturate(1.0 - r);
        shape = saturate(core + (streakH + streakV) * 0.85 + petals * 0.5);
    }
    else // KIND_GLOW（既定・後方互換）
    {
        float r2 = dot(uv, uv);
        float fall = saturate(1.0 - r2);
        shape = pow(fall, params.y);
    }
}

// ---- 歪みパーティクル PS（RG16F 歪みオフセットバッファへ加算）----
// color.r = 歪み強度, color.a = 寿命フェード。出力 rg = ローカルUV単位のオフセット。
// KIND_RING = 拡大する衝撃波（放射方向へ押し出す）/ その他 = 熱ゆらぎ（fbm ノイズベクトル）。
float4 PSDistort(VSOutput i) : SV_TARGET
{
    // シーン深度による手動オクルージョン（壁の向こうを歪ませない）
    float soft = 1.0;
    if (params2.z > 0.0)
    {
        float2 suv = i.pos.xy * float2(params2.z, params2.w);
        float sceneD = gSceneDepth.SampleLevel(sDepth, suv, 0).r;
        float sceneZ = params2.y / (sceneD - params2.x);
        soft = saturate((sceneZ - i.viewZ) / 0.5);
    }
    float strength = i.color.r * i.color.a * soft;
    clip(strength - 1e-4);

    float2 uv = i.uv;
    float  r  = length(uv);
    float2 dir = (r > 1e-4) ? uv / r : float2(0.0, 0.0);
    float2 ofs;
    if (i.kind == KIND_RING)
    {
        // 拡大リング: age01 で半径が広がる帯の内側→外側へ押し出す
        float rad  = i.age01;
        float band = exp(-pow((r - rad) * 7.0, 2.0));
        ofs = dir * band;
    }
    else
    {
        // 熱ゆらぎ: 上昇スクロールする fbm のベクトル場（丸マスク）
        float2 p = uv * 2.5 + float2(i.seed * 7.0, -params.z * 1.6 - i.seed * 3.0);
        float n1 = fbm2(p, 3);
        float n2 = fbm2(p + float2(9.7, 3.1), 3);
        float mask = saturate(1.0 - r);
        ofs = float2(n1, n2) * mask;
    }
    // 0.03 = 強度1.0 のときの最大オフセット（ローカルUVの約3%）
    return float4(ofs * strength * 0.03, 0.0, 1.0);
}

float4 PSMain(VSOutput i) : SV_TARGET
{
    // ---- soft particles を先に評価: シーン深度で手動オクルージョン＋接地フェード ----
    // 隠れた／寿命末端でフェードした画素はここで clip し、高コストな fbm/ShadeKind と
    // ROP 書き込みを丸ごとスキップする（加算は0加算・前乗算αは0書きで結果は同一）。
    float soft = 1.0;
    if (params2.z > 0.0)
    {
        float2 suv = i.pos.xy * float2(params2.z, params2.w);
        float sceneD = gSceneDepth.SampleLevel(sDepth, suv, 0).r;
        float sceneZ = params2.y / (sceneD - params2.x);     // 線形ビュー深度
        float fadeDist = params.w * (i.kind == KIND_SMOKE ? 3.0 : 1.0);
        soft = saturate((sceneZ - i.viewZ) / max(fadeDist, 1e-3));
    }
    clip(soft * i.color.a - 1e-3);

    float shape;
    float3 kindColor;
    if (i.texIdx != kNoTexture)
    {
        // テクスチャ貼り付けモード: プロシージャル形状の代わりに実画像をビルボードへ。
        // uv は [-1,1] 中心原点なので [0,1] へ写像。色は頂点色(寿命カーブ)で乗算し、
        // アルファをそのままカバレッジに使う（straight alpha、Sprite と同じ規約）。
        float2 tuv = i.uv * 0.5 + 0.5;
        float4 tex = gAlbedo.Sample(sDepth, tuv);
        shape = tex.a;
        kindColor = tex.rgb * i.color.rgb;
    }
    else
    {
        ShadeKind(i, shape, kindColor);
    }

    float cov = shape * i.color.a * soft;
    float3 rgb = kindColor * cov * params.x;     // 前乗算（加算 / 前乗算アルファ 両PSO共用）
    return float4(rgb, cov);
}
