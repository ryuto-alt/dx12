// @group 水
// ============================================================================
// Water.hlsl — 池・水路・プールの水面
//
// ■ 使い方
//   1. 平面メッシュを置く。**頂点が多いほど波が立つ**（板ポリの 4 頂点だと
//      波打たず、色とギラつきだけになる。分割した平面を使うこと）
//   2. Inspector の Shader にこれを割り当て、**アルファブレンドを ON**
//   3. パラメータは Inspector / Lua / Trigger から動かせる
//
// ■ パラメータ
//   effectValue      @range(0,1)  波の大きさ（0=凪、1=荒れる）
//   shaderParams.x   @range(0,4)  波の細かさ（大きいほど小さな波。既定 1）
//   shaderParams.y   @range(0,2)  流れの速さ
//   shaderParams.z   @range(0,1)  濁り（大きいほど深い色）
//   shaderParams.w   @range(0,1)  泡の量
//
// ■ 既知の割り切り
//   背景の屈折（水越しに向こうが歪む）と浅瀬の泡（岸に近いほど白い）は未対応。
//   どちらも「不透明を描き終えた画面のコピー」と「シーン深度」が要るため、
//   エンジン側にそのパスを足すまで出せない。
// ============================================================================
#include "UnoCustom.hlsli"

static const float3 kShallow = float3(0.10, 0.36, 0.42);
static const float3 kDeep    = float3(0.015, 0.075, 0.14);
static const float3 kFoam    = float3(0.88, 0.94, 0.97);

// 3 波の合成。向きと波長をずらさないと縞にしか見えない。
// 戻り値 .xyz = 変位、.w = 峰らしさ
float4 WaterWaves(float3 wp, float amp, float scale, float speed)
{
    float4 w0 = UnoGerstner(wp, float2( 1.00,  0.35), 6.0 / scale, 0.32 * amp, 1.30 * speed, time);
    float4 w1 = UnoGerstner(wp, float2(-0.60,  1.00), 3.7 / scale, 0.26 * amp, 1.05 * speed, time);
    float4 w2 = UnoGerstner(wp, float2( 0.25, -1.00), 1.9 / scale, 0.18 * amp, 0.80 * speed, time);
    return float4(w0.xyz + w1.xyz + w2.xyz, (w0.w + w1.w + w2.w) / 3.0);
}

PSInput VSMain(VSInput i)
{
    const float amp   = saturate(effectValue);
    const float scale = max(shaderParams.x, 0.05);
    const float speed = max(shaderParams.y, 0.0);

    // 波はワールド座標で作る（板を並べても継ぎ目が出ない）。
    const float3 wp = UnoLocalToWorld(i.position);
    const float4 w  = WaterWaves(wp, amp, scale, speed);

    // 法線は近傍差分。解析微分より短く、波を足しても崩れない。
    const float e  = 0.35;
    const float hC = w.y;
    const float hX = WaterWaves(wp + float3(e, 0, 0), amp, scale, speed).y;
    const float hZ = WaterWaves(wp + float3(0, 0, e), amp, scale, speed).y;
    const float3 n = normalize(float3(hC - hX, e, hC - hZ));

    // ★ワールドで動かしたので mvp ではなく view*proj を通す（mvp は model 込みのため二重になる）。
    return UnoVSFromWorld(wp + w.xyz, n, i.texCoord);
}

float4 PSMain(PSInput i) : SV_TARGET
{
    const float3 v     = normalize(cameraPos - i.worldPos);
    const float  murk  = saturate(shaderParams.z);
    const float  foam  = saturate(shaderParams.w);
    const float  scale = max(shaderParams.x, 0.05);
    const float  speed = max(shaderParams.y, 0.0);

    // 頂点では出せない粒度のさざ波をここで足す。
    const float2 uv = i.worldPos.xz * (0.55 * scale);
    const float  r1 = UnoFbm(uv + float2(0.0, time * 0.07 * speed), 3);
    const float  r2 = UnoFbm(uv * 2.1 - float2(time * 0.05 * speed, 0.0), 3);
    const float3 n  = normalize(i.worldNormal + float3((r1 - 0.5) * 0.55, 0.0, (r2 - 0.5) * 0.55));

    // 見込み角で浅い色と深い色を混ぜる。真上は深く、寝かせると空を映す。
    const float  f    = UnoFresnel(n, v, 0.02, 5.0);
    const float3 body = lerp(kShallow, kDeep, murk);
    // ★空の色は固定色 + 太陽の色味。lightColor をそのまま掛けると白飛びする。
    const float3 sky  = lerp(float3(0.34, 0.46, 0.58), UnoSunTint(), 0.25);
    float3 col = lerp(body, sky, f);

    // 太陽のギラつき。水面らしさの大半はここ。
    col += UnoSunTint() * UnoSunSpecular(n, v, 220.0) * 1.2;

    // 峰の泡（しきい値は高め。低いと水面全部が白くなる）
    const float crest = saturate((r1 + r2) * 0.5 * 2.2 - 1.15);
    col = lerp(col, kFoam, crest * foam);

    // 寝かせるほど不透明＝遠くの水面は向こうが見えない。水らしさの要。
    float a = lerp(0.55, 0.96, f);
    a = max(a, crest * foam);
    return float4(col, saturate(a));
}
