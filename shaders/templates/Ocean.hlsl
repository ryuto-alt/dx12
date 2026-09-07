// @group 水
// ============================================================================
// Ocean.hlsl — 海。うねりが大きく、白波が立ち、遠くほど空へ溶ける
//
//   Water.hlsl との違いは「規模」。波長を長く、うねりを複数重ね、
//   峰に白波（whitecap）を乗せ、遠景を大気色へ寄せて水平線を作る。
//
// ■ 使い方
//   1. 大きめの平面メッシュを置く（**分割数が多いほど良い**。4 頂点だと波が出ない）
//   2. Shader にこれを割り当て、**アルファブレンドを ON**
//   3. 空/環境の色に合わせて shaderParams.z（大気色の寄せ）を調整する
//
// ■ パラメータ
//   effectValue      @range(0,1)  海の荒さ（0=べた凪、1=時化）
//   shaderParams.x   @range(0,3)  うねりの規模（小さいほど波長が長い＝広い海）
//   shaderParams.y   @range(0,2)  進む速さ
//   shaderParams.z   @range(0,1)  遠景を大気色へ寄せる量（水平線の作り）
//   shaderParams.w   @range(0,1)  白波の量
//
// ■ 既知の割り切り
//   屈折と浅瀬の泡は Water.hlsl と同じ理由で未対応。
//   海底が見える浅い海には Water.hlsl の方が向く。
// ============================================================================
#include "UnoCustom.hlsli"

static const float3 kOceanShallow = float3(0.04, 0.20, 0.26);
static const float3 kOceanDeep    = float3(0.005, 0.035, 0.075);
static const float3 kWhitecap     = float3(0.92, 0.96, 0.99);

// うねり 4 本。長い波から短い波へ。向きを散らして「面」にする。
float4 OceanWaves(float3 wp, float rough, float scale, float speed)
{
    float4 w0 = UnoGerstner(wp, float2( 1.00,  0.20), 42.0 / scale, 0.42 * rough, 2.60 * speed, time);
    float4 w1 = UnoGerstner(wp, float2( 0.70, -0.72), 23.0 / scale, 0.34 * rough, 2.10 * speed, time);
    float4 w2 = UnoGerstner(wp, float2(-0.35,  1.00), 11.0 / scale, 0.26 * rough, 1.60 * speed, time);
    float4 w3 = UnoGerstner(wp, float2(-1.00, -0.30),  5.5 / scale, 0.18 * rough, 1.20 * speed, time);
    return float4(w0.xyz + w1.xyz + w2.xyz + w3.xyz,
                  (w0.w + w1.w + w2.w + w3.w) * 0.25);
}

PSInput VSMain(VSInput i)
{
    const float rough = saturate(effectValue);
    const float scale = max(shaderParams.x, 0.05);
    const float speed = max(shaderParams.y, 0.0);

    const float3 wp = UnoLocalToWorld(i.position);
    const float4 w  = OceanWaves(wp, rough, scale, speed);

    // うねりが大きいぶん差分幅も広めに取る（狭いと法線がギラついて破綻する）。
    const float e  = 1.2;
    const float hC = w.y;
    const float hX = OceanWaves(wp + float3(e, 0, 0), rough, scale, speed).y;
    const float hZ = OceanWaves(wp + float3(0, 0, e), rough, scale, speed).y;
    const float3 n = normalize(float3(hC - hX, e, hC - hZ));

    PSInput o = UnoVSFromWorld(wp + w.xyz, n, i.texCoord);
    // 峰らしさを PS へ渡す（白波の判定に使う）。UV の空きチャンネルに載せる。
    o.texCoord = float2(i.texCoord.x, w.w);
    return o;
}

float4 PSMain(PSInput i) : SV_TARGET
{
    const float3 v     = normalize(cameraPos - i.worldPos);
    const float  haze  = saturate(shaderParams.z);
    const float  cap   = saturate(shaderParams.w);
    const float  scale = max(shaderParams.x, 0.05);
    const float  speed = max(shaderParams.y, 0.0);
    const float  crestV = i.texCoord.y;   // VS から来た峰らしさ

    // 細かい表面の乱れ。海は面が広いので、粗いノイズだけだとのっぺりする。
    const float2 uv = i.worldPos.xz * (0.13 * scale);
    const float  r1 = UnoFbm(uv + float2(0.0, time * 0.05 * speed), 4);
    const float  r2 = UnoFbm(uv * 2.7 - float2(time * 0.04 * speed, 0.0), 3);
    const float3 n  = normalize(i.worldNormal + float3((r1 - 0.5) * 0.35, 0.0, (r2 - 0.5) * 0.35));

    const float  f    = UnoFresnel(n, v, 0.02, 5.0);
    const float3 body = lerp(kOceanShallow, kOceanDeep, 0.50);
    // ★空の色は固定色を基準にし、太陽の「色味」だけ少し混ぜる。
    //   lightColor をそのまま使うと太陽の強度で白飛びする（UnoSunTint のコメント参照）。
    const float3 sky  = lerp(float3(0.26, 0.38, 0.52), UnoSunTint(), 0.18);
    // ★反射をフレネルそのままで混ぜると、低い視点ではほぼ全面が空色になって
    //   「白い板」に見える（実測）。0.85 掛けで水の色を必ず残す。
    float3 col = lerp(body, sky, f * 0.85);

    // 太陽の道（サングリッター）。海面で一番目を引く要素。
    col += UnoSunTint() * UnoSunSpecular(n, v, 320.0) * 1.5;

    // 白波は「峰 かつ 表面が荒れている」ところだけ。峰だけだと縞になる。
    // ★しきい値は高めに。低いと海面全部が泡になって真っ白な絨毯にしか見えない。
    const float foamMask = saturate(crestV * 2.4 - 1.30) * saturate(r1 * 2.0 - 0.85);
    col = lerp(col, kWhitecap, saturate(foamMask * 3.0) * cap);

    // 遠いほど大気色へ寄せる＝水平線が溶ける。距離は視点からの実距離で取る。
    const float dist = length(cameraPos - i.worldPos);
    const float fog  = saturate(dist / 900.0) * haze;
    col = lerp(col, sky, fog);

    // ★海はほぼ不透明にする。透けさせると背後（エディタのグリッドや地面）が
    //   見えて「白い薄膜」にしか見えない。水中が見えるべきなのは浅い水＝Water.hlsl の担当。
    float a = lerp(0.94, 1.0, f);
    a = max(a, saturate(foamMask * 3.0) * cap);
    return float4(col, saturate(a));
}
