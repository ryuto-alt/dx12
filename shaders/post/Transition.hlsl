// シーントランジションのフルスクリーンオーバーレイ。
// progress(0→1→0) を coverage として、type 別にオーバーレイのアルファを計算する。
//
// ★type の一覧と各分岐の式は 3 箇所に写しがある。片方だけ直すと
//   「エディタのサムネイルと実物が違う」になる:
//     - src/renderer/SceneTransition.h  … enum TransitionType（値の正）
//     - このファイル                     … 実物の描画
//     - src/editor/TransitionSwatch.h    … エディタのサムネイル（ImDrawList で同じ絵を描く）
//   | 値 | 型             | 覆い方                                   |
//   |----|----------------|------------------------------------------|
//   | 0  | FadeBlack      | 黒アルファ = progress                    |
//   | 1  | Wipe           | uv.x < progress                          |
//   | 2  | Circle         | 中心からの距離 > 1-progress              |
//   | 3  | WipeVertical   | uv.y < progress                          |
//   | 4  | Seek           | 斜めエッジ + プレイヘッド + シークバー   |
//   | 5  | FadeWhite      | 白アルファ = progress                    |
//   | 6  | Blinds         | 8 本の横帯が各々 progress ぶん閉じる     |
//   | 7  | RadialClock    | 12時から時計回りの角度 < progress        |
//   | 8  | Diamond        | 中心からのマンハッタン距離 > 1-progress  |

#include "FullscreenTri.hlsli"

cbuffer TransCB : register(b0)
{
    float progress;  // 0..1 の被覆率
    int   type;      // 上の対応表を参照
    float aspect;    // 画面アスペクト w/h
    float total;     // 遷移全体の進行 0..1（閉じ→開きを跨ぐ。type4 のシークバー用）
};

// ブラインド(type 6)の帯の本数。TransitionSwatch.h の kBlindCount と同じ値にすること。
static const float kBlindCount = 8.0;

// ">" チェビロン1枚。cpos=先端x位置。先端は進行方向(右)を向く
float Chevron(float2 uv, float cpos)
{
    float ay = abs(uv.y - 0.5);
    if (ay > 0.10) return 0.0;
    float edge = cpos - ay * 0.55;              // 山型の稜線
    float d = abs(uv.x - edge);
    return smoothstep(0.014, 0.004, d) * smoothstep(0.10, 0.03, ay);
}

float4 TransPS(FSQuadVSOut i) : SV_TARGET
{
    float a = 0.0;
    // 暗幕の色。ホワイトアウト(type 5)だけ白、ほかは黒。
    float3 curtain = float3(0.0, 0.0, 0.0);

    if (type == 0)
    {
        a = progress;
    }
    else if (type == 1)
    {
        a = (i.uv.x < progress) ? 1.0 : 0.0;
    }
    else if (type == 2)
    {
        float2 d = i.uv - 0.5;
        d.x *= aspect;
        float maxd = length(float2(0.5 * aspect, 0.5));
        float dist = length(d) / maxd;
        a = (dist > (1.0 - progress)) ? 1.0 : 0.0;
    }
    else if (type == 5)
    {
        // ホワイトアウト: フェードと同じ形で色だけ白（閃光・回想の入り）
        a = progress;
        curtain = float3(1.0, 1.0, 1.0);
    }
    else if (type == 6)
    {
        // ブラインド: 帯を kBlindCount 本に割り、各帯が上端から progress ぶん閉じる。
        // frac は帯内のローカル 0..1（＝全部の帯が同時に同じだけ閉じる）。
        a = (frac(i.uv.y * kBlindCount) < progress) ? 1.0 : 0.0;
    }
    else if (type == 7)
    {
        // 時計ワイプ: 12時(上)から時計回りに扇が伸びる。
        // atan2(x, -y) は上向きが 0、右回りが正になる（画面の v は下向きが +）。
        float2 d = i.uv - 0.5;
        d.x *= aspect;
        float ang = atan2(d.x, -d.y);                       // -pi..pi
        float t01 = (ang < 0.0) ? (ang + 6.2831853) / 6.2831853
                                : ang / 6.2831853;          // 0..1（12時起点・時計回り）
        a = (t01 < progress) ? 1.0 : 0.0;
    }
    else if (type == 8)
    {
        // 菱形アイリス: 円(type 2)の距離をマンハッタン距離に替えただけ。
        float2 d = abs(i.uv - 0.5);
        d.x *= aspect;
        float maxd = 0.5 * aspect + 0.5;                    // 角までのマンハッタン距離
        float dist = (d.x + d.y) / maxd;
        a = (dist > (1.0 - progress)) ? 1.0 : 0.0;
    }
    else if (type == 4)
    {
        // シークバー早送り: 光るプレイヘッドが左→右へ2回掃く(閉じで覆い、開きで新シーンを開放)。
        // 画面下に動画プレイヤー風シークバーが全体進行と同期して伸びる。
        const float3 gold  = float3(1.0, 0.62, 0.15);   // タイトルの琥珀ゴールドと同系
        const bool  closing = (total < 0.5);
        float front = closing ? progress : (1.0 - progress);   // 両フェーズとも 0→1 で右進行
        float x = i.uv.x + (i.uv.y - 0.5) * 0.10;              // エッジは少し斜め(疾走感)

        // 暗幕(通過済み側): 深いネイビー + かすかな走査線
        float covered = closing ? smoothstep(front + 0.002, front - 0.004, x)
                                : smoothstep(front - 0.002, front + 0.004, x);
        float3 col = float3(0.016, 0.020, 0.048);
        col += 0.015 * step(0.5, frac(i.uv.y * 90.0));
        a = covered;

        // プレイヘッドのグロー(覆われた側に尾を引く)
        float behind = closing ? (front - x) : (x - front);
        float glow = (behind > 0.0) ? exp(-behind * 26.0) : 0.0;
        col += gold * glow * 1.6;

        // ">>" チェビロン2枚がヘッドを追走(覆われた側)
        float dirSign = closing ? 1.0 : -1.0;
        float chev = Chevron(i.uv, front - dirSign * 0.050)
                   + Chevron(i.uv, front - dirSign * 0.095) * 0.6;
        col += gold * chev * covered * 1.2;

        // シークバー(トラック+充填+ノブ)。未被覆領域にも薄く重ねる
        float barY = 0.935;
        float inBar = smoothstep(0.0045, 0.0035, abs(i.uv.y - barY));
        if (inBar > 0.0)
        {
            float fill = smoothstep(total + 0.002, total - 0.002, i.uv.x);
            float3 barCol = lerp(float3(0.35, 0.35, 0.38), gold, fill);
            col = lerp(col, barCol, inBar);
            a = max(a, inBar * 0.85);
        }
        float2 kd = i.uv - float2(total, barY);
        kd.x *= aspect;
        float knob = smoothstep(0.009, 0.006, length(kd));
        if (knob > 0.0)
        {
            col = lerp(col, gold * 1.4, knob);
            a = max(a, knob);
        }
        return float4(col, a);
    }
    else
    {
        // type 3（縦ワイプ）と、未知の値のフォールバック。
        a = (i.uv.y < progress) ? 1.0 : 0.0;
    }
    return float4(curtain, a);
}
