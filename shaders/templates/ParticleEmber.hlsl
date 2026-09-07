// @group パーティクル
// ============================================================================
// ParticleEmber.hlsl — 燃えさし。芯が白熱し、外へ向かって橙→赤へ落ちて消える
//
//   自作パーティクルシェーダーの雛形。既定の kind と違って
//   「粒の中の温度勾配」を持たせてあるので、火の粉・残り火・溶けた金属の飛沫に向く。
//
// ■ 使い方
//   1. パーティクル放出器のレイヤーの Shader にこれを割り当てる
//   2. ブレンドは 加算(Additive) 推奨。α にすると煙寄りの見え方になる
//   3. 色は放出器側の color / colorEnd がそのまま入ってくる（i.color）
//
// ■ 出力は前乗算アルファ
//   色に α を掛けてから返すこと。掛け忘れると加算でふちが四角く光る。
// ============================================================================
#include "UnoParticle.hlsli"

VSOutput VSMain(VSInput i)
{
    return UnoParticleVS(i);
}

float4 PSMain(VSOutput i) : SV_TARGET
{
    const float r = length(i.uv);
    if (r > 1.0) discard;                       // 四角の角を落とす（無駄な塗りを減らす）

    // 個体ごとにわずかに形を崩す。全部同じ丸だと「粒」ではなく「点」に見える。
    const float wob = fbm2_01(i.uv * 2.6 + i.seed * 31.7 + UnoTime() * 0.8, 3);
    const float rr  = r * (0.85 + 0.30 * wob);

    // 芯 → 外の減衰。芯は硬く、外はふわっと。
    const float core = saturate(1.0 - rr * 1.9);
    const float halo = saturate(1.0 - rr);
    const float body = core * core + halo * halo * 0.35;

    // 温度: 芯ほど白い。寿命が進むほど全体が冷える。
    const float cool = i.age01 * i.age01;                 // 後半で一気に冷える
    const float temp = saturate(core * 1.6 * (1.0 - cool * 0.85));
    float3 col = i.color.rgb;
    col = lerp(col, float3(1.0, 0.96, 0.88), temp);       // 芯を白熱させる

    // 明滅。個体ごとに位相をずらさないと全部が同時に瞬いて機械的になる。
    const float flick = 0.85 + 0.15 * sin(UnoTime() * 26.0 + i.seed * 41.0);

    // 寿命の終わりで消える（age01=1 でちょうど 0）
    const float fade = saturate(1.0 - cool);

    const float a = body * fade * flick * i.color.a * UnoSoftParticle(i);
    return float4(col * a, a);                            // 前乗算
}
