// トランジション専用プレビュー（エディタの「トランジション」窓 + プリセットのサムネイル）。
//
// ★なぜ専用画面が要るか: 遷移を編集中のシーンの上で再生すると、暗幕の形と
//   シーンの絵が混ざって「どこが幕でどこが背景か」が読み取れない。しかも中間点で
//   本当にシーンが切り替わるわけではないので「切り替わった感」も分からない。
//   ここでは中身の分かっている **架空のゲーム画面 2 枚**（昼のフィールド → 夜のダンジョン）を
//   描き、遷移の中間点で A から B へ差し替える。＝ 幕の形と、切り替わりの瞬間の
//   両方が一目で分かる。
//
// ★暗幕の形は shaders/post/TransitionCurtain.hlsli（実機と完全に同じ関数）を呼ぶだけ。
//   プレビュー用の近似実装は持たない＝「プレビューと実物が違う」が原理的に起きない。
//
// 同じ PSO をプリセットのサムネイル（アトラスへタイル単位のビューポートで描く）にも使う。

#include "FullscreenTri.hlsli"
#include "TransitionCurtain.hlsli"

cbuffer PreviewCB : register(b0)
{
    float progress;  // 0..1 の被覆率
    int   type;      // TransitionType
    float aspect;    // タイル/プレビューのアスペクト w/h
    float total;     // 遷移全体の進行 0..1（0.5 未満 = シーンA / 以降 = シーンB）
};

// ---- 小道具 ----------------------------------------------------------------

float TrpBox(float2 uv, float2 c, float2 h)
{
    float2 d = abs(uv - c) - h;
    return smoothstep(0.0035, -0.0035, max(d.x, d.y));
}

float TrpDisc(float2 uv, float2 c, float r, float asp)
{
    float2 d = uv - c;
    d.x *= asp;
    return smoothstep(r, r * 0.88, length(d));
}

float TrpRing(float2 uv, float2 c, float r, float w, float asp)
{
    float2 d = uv - c;
    d.x *= asp;
    return smoothstep(w, w * 0.4, abs(length(d) - r));
}

// 幹 + 円錐の木（昼のフィールド用）
float3 TrpTree(float3 col, float2 uv, float2 basePos, float h, float asp)
{
    float trunk = TrpBox(uv, float2(basePos.x, basePos.y - h * 0.16), float2(h * 0.045 / asp, h * 0.16));
    col = lerp(col, float3(0.24, 0.16, 0.10), trunk);
    // 三角形 3 枚を重ねた針葉樹
    [unroll]
    for (int k = 0; k < 3; ++k)
    {
        float t  = float(k) * 0.22;
        float ty = basePos.y - h * (0.30 + t);          // この段の中心 y
        float tw = h * (0.30 - t * 0.55) / asp;         // 底辺の半幅
        float th = max(h * 0.26, 1e-4);                 // 段の半分の高さ
        float2 d = uv - float2(basePos.x, ty);
        float  ny = saturate((d.y + th) / (2.0 * th));  // 0=頂点 / 1=底辺
        float  m  = step(abs(d.y), th)
                  * smoothstep(0.004, -0.004, abs(d.x) - tw * ny);
        col = lerp(col, float3(0.13, 0.34, 0.16) * (0.85 + 0.15 * float(k)), m);
    }
    return col;
}

// ゲームらしさの決め手は HUD。A/B で色と残量を変える＝切り替わりが一目で分かる。
float3 TrpHud(float3 col, float2 uv, float asp, float3 barCol, float hp, float mp, int slot)
{
    // 左上: HP / MP ゲージ
    float2 hpC = float2(0.150, 0.070), hpH = float2(0.110, 0.017);
    float2 mpC = float2(0.130, 0.108), mpH = float2(0.090, 0.011);
    col = lerp(col, float3(0.04, 0.04, 0.06), TrpBox(uv, hpC, hpH + 0.006) * 0.85);
    col = lerp(col, barCol,
               TrpBox(uv, hpC, hpH) * step(uv.x, hpC.x - hpH.x + hpH.x * 2.0 * hp));
    col = lerp(col, float3(0.04, 0.04, 0.06), TrpBox(uv, mpC, mpH + 0.005) * 0.85);
    col = lerp(col, float3(0.25, 0.55, 0.95),
               TrpBox(uv, mpC, mpH) * step(uv.x, mpC.x - mpH.x + mpH.x * 2.0 * mp));

    // 右上: ミニマップ
    float2 mmC = float2(0.885, 0.135);
    col = lerp(col, float3(0.05, 0.06, 0.09), TrpDisc(uv, mmC, 0.085, asp) * 0.80);
    col = lerp(col, float3(0.45, 0.50, 0.60), TrpRing(uv, mmC, 0.085, 0.006, asp));
    col = lerp(col, barCol, TrpDisc(uv, mmC, 0.012, asp));

    // 下中央: ホットバー 5 枠（1 枠だけ選択中）
    [unroll]
    for (int k = 0; k < 5; ++k)
    {
        float2 c = float2(0.5 + (float(k) - 2.0) * 0.062, 0.905);
        col = lerp(col, float3(0.06, 0.07, 0.10), TrpBox(uv, c, float2(0.026, 0.026 * asp)) * 0.88);
        float sel = (k == slot) ? 1.0 : 0.0;
        col = lerp(col, barCol * 0.9,
                   (TrpBox(uv, c, float2(0.026, 0.026 * asp))
                  - TrpBox(uv, c, float2(0.021, 0.021 * asp))) * (0.35 + 0.65 * sel));
    }
    return col;
}

// ---- 架空のゲーム画面 A: 昼のフィールド -------------------------------------

float3 TrpSceneA(float2 uv, float asp)
{
    const float horizon = 0.60;

    // 空
    float3 col = lerp(float3(0.22, 0.46, 0.80), float3(0.74, 0.87, 0.96),
                      saturate(uv.y / horizon));
    // 雲
    float cloud = TrFbm(float2(uv.x * 4.0 * asp, uv.y * 7.0));
    col = lerp(col, float3(1.0, 1.0, 1.0),
               smoothstep(0.58, 0.78, cloud) * saturate(1.0 - uv.y / horizon) * 0.85);
    // 太陽
    float2 sunC = float2(0.795, 0.155);
    col += float3(1.0, 0.92, 0.65) * TrpDisc(uv, sunC, 0.042, asp);
    float2 sd = uv - sunC; sd.x *= asp;
    col += float3(1.0, 0.85, 0.50) * exp(-length(sd) * 9.0) * 0.35;

    // 遠景の山
    float ridge = horizon - 0.075 - 0.045 * sin(uv.x * 7.3) - 0.028 * sin(uv.x * 15.1 + 1.2);
    col = lerp(col, float3(0.40, 0.50, 0.62), smoothstep(-0.004, 0.004, uv.y - ridge)
                                             * step(uv.y, horizon + 0.001));
    // 手前の丘
    float ridge2 = horizon - 0.028 - 0.030 * sin(uv.x * 4.1 + 2.0);
    col = lerp(col, float3(0.21, 0.42, 0.24), smoothstep(-0.004, 0.004, uv.y - ridge2)
                                             * step(uv.y, horizon + 0.001));

    // 地面（奥ほど暗く）
    float g = smoothstep(-0.004, 0.004, uv.y - horizon);
    float3 ground = lerp(float3(0.16, 0.34, 0.18), float3(0.30, 0.52, 0.26),
                         saturate((uv.y - horizon) / (1.0 - horizon)));
    ground *= 0.88 + 0.24 * TrFbm(float2(uv.x * 12.0 * asp, uv.y * 22.0));
    col = lerp(col, ground, g);

    // 道（奥へ細くなる）
    float depth = saturate((uv.y - horizon) / (1.0 - horizon));
    float road  = smoothstep(0.012 + depth * 0.20, 0.0, abs(uv.x - 0.5)) * g;
    col = lerp(col, float3(0.56, 0.48, 0.34), road * 0.85);

    // 木
    col = TrpTree(col, uv, float2(0.185, 0.735), 0.30, asp);
    col = TrpTree(col, uv, float2(0.845, 0.700), 0.24, asp);
    col = TrpTree(col, uv, float2(0.320, 0.655), 0.16, asp);

    // プレイヤー（カプセルの影絵）
    float2 pc = float2(0.500, 0.735);
    float body = TrpBox(uv, pc, float2(0.020, 0.058))
               + TrpDisc(uv, pc - float2(0.0, 0.062), 0.026, asp);
    col = lerp(col, float3(0.10, 0.12, 0.16), saturate(body));
    col = lerp(col, float3(0.09, 0.20, 0.11), TrpDisc(uv, pc + float2(0.0, 0.062), 0.030, asp) * 0.55);

    col = TrpHud(col, uv, asp, float3(0.90, 0.26, 0.24), 0.78, 0.55, 1);
    return col;
}

// ---- 架空のゲーム画面 B: 夜のダンジョン -------------------------------------

float3 TrpSceneB(float2 uv, float asp)
{
    // 天井〜奥のかすみ
    float3 col = lerp(float3(0.030, 0.032, 0.070), float3(0.075, 0.070, 0.115),
                      saturate(uv.y * 1.4));

    // 星（天井の隙間）
    float2 sg = floor(uv * float2(70.0 * asp, 40.0));
    float  sh = TrHash21(sg);
    col += float3(0.85, 0.90, 1.0) * step(0.985, sh) * saturate(1.0 - uv.y * 3.2) * 0.9;

    // 廊下の奥の出口（冷たい光）。★左右の壁の「間」に置くこと。以前ここに月を
    //   x=0.215 で描いていたが、その x は壁の内側なので毎フレーム壁に塗り潰されて
    //   一度も見えていなかった（描いているのに映らない絵になっていた）。
    float2 exitC = float2(0.5, 0.235);
    float2 ed = uv - exitC; ed.x *= asp;
    col += float3(0.35, 0.50, 0.90) * exp(-length(ed) * 7.5) * 0.55;
    col += float3(0.72, 0.84, 1.00) * TrpDisc(uv, exitC, 0.030, asp) * 0.85;

    // 廊下（左右の壁が奥へ収束）
    float depth  = saturate(uv.y);
    float inner  = 0.185 + 0.155 * depth;                 // 壁の内側の x（中心から）
    float wallL  = smoothstep(0.004, -0.004, uv.x - (0.5 - inner));
    float wallR  = smoothstep(0.004, -0.004, (0.5 + inner) - uv.x);
    float3 stone = float3(0.115, 0.105, 0.135) * (0.75 + 0.5 * TrFbm(uv * float2(9.0 * asp, 14.0)));
    // 石積みの目地
    float brick  = step(0.5, frac(uv.y * 13.0)) * 0.5 + 0.5;
    stone *= 0.85 + 0.30 * step(0.06, frac(uv.y * 13.0)) * step(0.06, frac(uv.x * 9.0 * asp + brick));
    col = lerp(col, stone, saturate(wallL + wallR));

    // 床（奥行きのある市松）
    float floorY = 0.615;
    float f = smoothstep(-0.004, 0.004, uv.y - floorY);
    float pd = max(1.0 - (uv.y - floorY) * 2.2, 0.08);     // 遠近の圧縮
    float2 fc = float2((uv.x - 0.5) / pd, 1.0 / pd);
    float chk = fmod(floor(fc.x * 7.0) + floor(fc.y * 2.2), 2.0);
    float3 floorCol = lerp(float3(0.085, 0.080, 0.100), float3(0.140, 0.130, 0.155), chk);
    floorCol *= saturate(0.35 + (uv.y - floorY) * 1.9);
    col = lerp(col, floorCol, f);

    // 松明（左右）
    float2 t0 = float2(0.155, 0.395), t1 = float2(0.845, 0.395);
    float2 d0 = uv - t0; d0.x *= asp;
    float2 d1 = uv - t1; d1.x *= asp;
    col += float3(1.00, 0.52, 0.16) * exp(-length(d0) * 5.2) * 0.85;
    col += float3(1.00, 0.52, 0.16) * exp(-length(d1) * 5.2) * 0.85;
    col += float3(1.00, 0.82, 0.45) * TrpDisc(uv, t0, 0.016, asp);
    col += float3(1.00, 0.82, 0.45) * TrpDisc(uv, t1, 0.016, asp);

    // プレイヤー（松明に照らされた影絵）
    float2 pc = float2(0.500, 0.735);
    float body = TrpBox(uv, pc, float2(0.020, 0.058))
               + TrpDisc(uv, pc - float2(0.0, 0.062), 0.026, asp);
    col = lerp(col, float3(0.16, 0.13, 0.12), saturate(body));
    col = lerp(col, float3(0.02, 0.02, 0.03), TrpDisc(uv, pc + float2(0.0, 0.062), 0.030, asp) * 0.6);

    // ビネット（洞窟らしさ）
    float2 vd = uv - 0.5; vd.x *= asp;
    col *= 1.0 - saturate(length(vd) * 1.15 - 0.30) * 0.85;

    col = TrpHud(col, uv, asp, float3(0.55, 0.35, 0.90), 0.41, 0.86, 3);
    return col;
}

// ---- 合成 ------------------------------------------------------------------

float4 TransPreviewPS(FSQuadVSOut i) : SV_TARGET
{
    // 中間点（画面が隠れきった瞬間）で A → B に差し替わる。実機のシーンロードと同じ位置。
    float3 scene = (total < 0.5) ? TrpSceneA(i.uv, aspect) : TrpSceneB(i.uv, aspect);

    float4 curtain = TransitionCurtain(i.uv, progress, total, type, aspect);
    float3 outCol  = lerp(scene, curtain.rgb, curtain.a);

    return float4(outCol, 1.0);
}
