// ShadowPcss.hlsli — PCSS（Percentage-Closer Soft Shadows）。
//
// 「ブロッカー探索 → 半影(ペナンブラ)幅の推定 → 可変幅 PCF」の 3 段。
// 接地部は鋭く、遮蔽物から離れるほど柔らかくなる（contact hardening）。
//
// ★カスケード境界で半影の太さが不連続にならない理由（設計の要）
//   CSM は正射なので、カスケード c のシャドウマップは
//     UV 1.0 = worldSize[c] メートル、深度 1.0 = worldSize[c] メートル
//   という同じスケールになっている（Application::ComputeCascades が
//   ortho 幅 = 2*radius、深度レンジ = 2*radius で作っているため）。
//   よって
//     penumbraWorld = (zR - zB) * worldSize * tanTheta
//     penumbraUV    = penumbraWorld / worldSize = (zR - zB) * tanTheta
//   と worldSize が約分され、**カスケードごとの補正が要らない**。
//   世界座標で見た半影の太さはカスケードをまたいでも連続する。
//   （NVIDIA の元論文の式は透視ライト前提なので、そのまま使うと平行光で破綻する）
//
// ★サンプリング
//   Vogel ディスク（黄金角スパイラル）+ Interleaved Gradient Noise による回転。
//   Vogel は「回転しても自分自身に写らない」性質があるので、ピクセルごとの回転で
//   バンディングがノイズへ変わる（Sterna 2018 / Godot / Unity HDRP と同じ流儀）。
//   TAA が有効なときだけ、フレーム連番 × 黄金比で位相をさらに回して時間方向に散らす。
//   TAA が無効なときにこれをやるとチラつくだけなので、C++ 側が位相 0 を送る。
//
// ★ブロッカー探索は SampleCmp が使えない（生の深度が要る）ので Load で直接読む。
//   探索半径を大きくしすぎると「一番効く遮蔽物を飛ばして影に穴が開く」ので、
//   既定は半影の上限テクセル数と同じにしてある。

#ifndef SHADOW_PCSS_HLSLI
#define SHADOW_PCSS_HLSLI

#define PCSS_BLOCKER_SAMPLES 16
#define PCSS_FILTER_SAMPLES  16

// 黄金角スパイラル（Vogel disk）。i/n を半径に、黄金角 2.399963 rad を角度に使う。
float2 PcssVogelDisk(int i, int n, float phi)
{
    float r     = sqrt((i + 0.5) / (float)n);
    float theta = i * 2.39996323 + phi;
    float s, c;
    sincos(theta, s, c);
    return float2(r * c, r * s);
}

// Interleaved Gradient Noise（Jorge Jimenez）。ピクセル座標だけで決まる空間ディザ。
float PcssIgn(float2 px)
{
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(px, magic.xy)));
}

// uv      : シャドウマップ UV（既にカスケードへ射影済み）
// zRecv   : 受光点の深度（バイアス適用済み）
// svPos   : SV_Position.xy（ディザ用）
// 戻り値  : 0=完全に影 / 1=完全に照らされている
float SampleShadowPcss(Texture2DArray shadowMap, int cascade,
                       float2 uv, float zRecv, float2 svPos)
{
    const float texel    = shadowParams.x;          // 1/shadowMapSize
    const float mapSize  = 1.0 / max(texel, 1e-8);
    const float tanTheta = pcssParams.x;
    const float maxPenUV = pcssParams.y * texel;
    const float searchUV = pcssParams.w * texel;

    // 空間ディザ（IGN）+ 時間ディザ（フレーム連番 × 黄金比。TAA 無効時は 0）
    const float phi = (PcssIgn(svPos) + pcssParams.z) * 6.28318531;

    // ---- 1) ブロッカー探索 ----
    float blockerSum   = 0.0;
    float blockerCount = 0.0;
    [unroll]
    for (int i = 0; i < PCSS_BLOCKER_SAMPLES; ++i)
    {
        float2 suv = uv + PcssVogelDisk(i, PCSS_BLOCKER_SAMPLES, phi) * searchUV;
        int2   px  = (int2)(saturate(suv) * mapSize);
        float  d   = shadowMap.Load(int4(px, cascade, 0)).r;
        if (d < zRecv)
        {
            blockerSum   += d;
            blockerCount += 1.0;
        }
    }
    // 遮蔽物なし = 完全に照らされている（ここで抜けるのが PCSS が実用速度で動く理由）
    if (blockerCount < 0.5) return 1.0;

    const float zBlocker = blockerSum / blockerCount;

    // ---- 2) 半影幅の推定 ----
    // 下限を 0.75 テクセルにしてあるのは、接地部で半影が 0 になると
    // 1 タップ PCF＝ハードシャドウのジャギーが出るため。
    const float penUV = clamp((zRecv - zBlocker) * tanTheta, texel * 0.75, maxPenUV);

    // ---- 3) 可変幅 PCF ----
    // 探索と同じ位相だとサンプル点が重なるので 1 rad ずらす。
    float s = 0.0;
    [unroll]
    for (int j = 0; j < PCSS_FILTER_SAMPLES; ++j)
    {
        float2 o = PcssVogelDisk(j, PCSS_FILTER_SAMPLES, phi + 1.0) * penUV;
        s += shadowMap.SampleCmpLevelZero(g_shadowSampler,
                 float3(uv + o, (float)cascade), zRecv);
    }
    return s / (float)PCSS_FILTER_SAMPLES;
}

// 従来の 3x3 PCF（PCSS 無効時。**旧実装と 1 命令も変えていない**）。
float SampleShadowPcf3x3(Texture2DArray shadowMap, int cascade, float2 uv, float zRecv)
{
    float texel = shadowParams.x;
    float s = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    [unroll]
    for (int x = -1; x <= 1; ++x)
        s += shadowMap.SampleCmpLevelZero(g_shadowSampler,
                 float3(uv + float2(x, y) * texel, (float)cascade), zRecv);
    return s / 9.0f;
}

// カスケード 1 枚ぶんの影サンプリング（PCSS / 3x3 PCF の切り替え込み）。
// ★pcssParams.x <= 0 のとき、絵は従来と完全に同じになる。
// bias は「受光面のシャドウアクネ/ピーターパン調整」。従来 Forward/Skinned/Terrain は
// shadowParams.y を引き、ForwardGrid だけ 0 だったので、その差をそのまま引数にしてある。
float SampleShadowCascadeCommon(Texture2DArray shadowMap, int cascade,
                                float3 worldPos, float2 svPos, float bias)
{
    float4 lc   = mul(float4(worldPos, 1.0f), cascadeViewProj[cascade]);
    float3 proj = lc.xyz / lc.w;
    float2 uv   = proj.xy * 0.5f + 0.5f;
    uv.y = 1.0f - uv.y;
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1) return 1.0f;

    float current = proj.z - bias;

    [branch]
    if (pcssParams.x > 0.0)
        return SampleShadowPcss(shadowMap, cascade, uv, current, svPos);
    return SampleShadowPcf3x3(shadowMap, cascade, uv, current);
}

#endif // SHADOW_PCSS_HLSLI
