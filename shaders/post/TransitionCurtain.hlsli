#ifndef DX12E_TRANSITION_CURTAIN_HLSLI
#define DX12E_TRANSITION_CURTAIN_HLSLI

// ===== シーントランジションの「暗幕」= 全型の唯一の実装 =====
//
// ★以前は「実物(Transition.hlsl) / エディタのサムネイル(TransitionSwatch.h)」の 2 箇所に
//   同じ形の写しがあり、片方だけ直すと「サムネイルと実物が違う」が起きていた。
//   いまは形の定義はこのファイルだけ。エディタのタイルもプレビュー窓も
//   TransitionPreview.hlsl 経由で**この関数そのもの**を GPU で走らせて描く。
//   ＝ 見えている絵が常に実物。C++ 側に形の写しは無い。
//
// ★enum の値は src/renderer/SceneTransition.h が正。settings.json とシーン JSON に
//   出る保存値なので既存の番号は動かさないこと（新しい型は末尾へ足す）。
//
//   | 値 | ID         | 名前             | 覆い方                                   |
//   |----|------------|------------------|------------------------------------------|
//   |  0 | fade       | 暗転             | 黒アルファ = progress                    |
//   |  1 | wipe       | 横ワイプ         | uv.x < progress                          |
//   |  2 | iris       | アイリス         | 中心からの距離 > 1-progress              |
//   |  3 | wipe_v     | 縦ワイプ         | uv.y < progress                          |
//   |  4 | seek       | シークバー早送り | 斜めエッジ + プレイヘッド + シークバー   |
//   |  5 | flash      | ホワイトアウト   | 白アルファ = progress                    |
//   |  6 | blinds     | ブラインド       | 横帯が各々 progress ぶん閉じる           |
//   |  7 | clock      | 時計ワイプ       | 12時から時計回りの角度 < progress        |
//   |  8 | diamond    | 菱形             | マンハッタン距離 > 1-progress            |
//   |  9 | wipe_diag  | 斜めワイプ       | (uv.x+uv.y)/2 < progress                 |
//   | 10 | curtain    | カーテン         | 左右の 2 枚が中央へ閉じる                |
//   | 11 | slide      | スライド         | 一枚板が右から入り、左へ抜ける           |
//   | 12 | blinds_v   | 縦ブラインド     | 縦帯が各々 progress ぶん閉じる           |
//   | 13 | star       | 星アイリス       | 5 芒星の外側                             |
//   | 14 | plus       | 十字アイリス     | 十字の外側                               |
//   | 15 | heart      | ハートアイリス   | ハート曲線の外側                         |
//   | 16 | dissolve   | ディゾルブ       | fbm ノイズ < progress                    |
//   | 17 | mosaic     | モザイク         | ブロック単位のランダム順で埋まる         |
//   | 18 | melt       | メルト           | 縦列が別々の速さで落ちる(Doom 融解)      |
//   | 19 | shatter    | ガラス割れ       | 三角の破片が順に現れる + ひびが光る      |
//   | 20 | glitch     | グリッチ         | 横帯がランダム順 + 色収差の縁            |
//   | 21 | hex        | ハニカム         | 六角セルが中心から広がる                 |
//   | 22 | checker    | チェッカー       | 市松の先攻マス→後攻マスの順で広がる      |
//   | 23 | spiral     | 渦巻き           | 角度 + 半径の渦で塗り潰す                |
//   | 24 | ripple     | 波紋             | 中心から広がる波打つ面 + 光るリング      |
//   | 25 | flood      | 水没             | 下から水位が上がる + 波打つ水面          |
//   | 26 | burn       | フィルムバーン   | ノイズの縁が燃えながら食う               |
//   | 27 | rush       | 集中線           | 白い集中線 + 外周から閉じる暗転          |

static const float TR_TAU = 6.28318530718;

// エッジのぼかし幅(uv 単位)。1080p で約 4px、160x90 のサムネイルで約 0.5px。
static const float TR_W = 0.0035;

// 帯・セルの数。ここを変えるとサムネイルも実物も同時に変わる(写しが無いので)。
static const float TR_BLIND_H = 8.0;    // ブラインド(横帯)
static const float TR_BLIND_V = 12.0;   // 縦ブラインド
static const float TR_MOSAIC  = 26.0;   // モザイクの横ブロック数
static const float TR_SHATTER = 9.0;    // ガラス割れの横セル数
static const float TR_GLITCH  = 26.0;   // グリッチの帯数
// ハニカムのスケール（小さいほどセルが大きい）。9.0 だと 158px のサムネイルでセルが
// つぶれて「六角形」に見えなかったので、六角形と分かる大きさまで落としている。
static const float TR_HEX     = 4.5;
static const float TR_CHECKER = 10.0;   // 市松の横マス数
static const float TR_MELT    = 48.0;   // メルトの縦列数
static const float TR_RUSH    = 96.0;   // 集中線の本数

// d>0 で 1、d<0 で 0。境界を TR_W ぶんだけぼかす。
float TrSoft(float d) { return smoothstep(-TR_W, TR_W, d); }

// progress を「0 で完全に開く / 1 で完全に閉じる」ように外へ w ぶん広げる。
// ぼかしを入れると端で塗り残し・消し残りが出るので、しきい値そのものを逃がしておく。
float TrPad(float p, float w) { return p * (1.0 + 2.0 * w) - w; }

float TrHash11(float n) { return frac(sin(n * 78.233) * 43758.5453123); }
float TrHash21(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453123); }

float TrValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = TrHash21(i);
    float b = TrHash21(i + float2(1.0, 0.0));
    float c = TrHash21(i + float2(0.0, 1.0));
    float d = TrHash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float TrFbm(float2 p)
{
    float v = 0.0;
    float amp = 0.5;
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        v += amp * TrValueNoise(p);
        p *= 2.03;
        amp *= 0.5;
    }
    return v;
}

// 画面中心から角までの距離(アスペクト補正後の uv 空間)。アイリス系の「1.0」。
float TrMaxRadius(float aspect) { return length(float2(0.5 * aspect, 0.5)); }

// 平頂六角形グリッド。戻り値 .xy = セル中心からの相対座標 / .zw = セル ID。
float4 TrHexCell(float2 p)
{
    const float2 s = float2(1.7320508, 1.0);
    p += 128.0;                                   // fmod を負にしない(HLSL の fmod は符号を残す)
    float2 a = fmod(p, s) - s * 0.5;
    float2 b = fmod(p - s * 0.5, s) - s * 0.5;
    bool   useA = dot(a, a) < dot(b, b);
    float2 gv = useA ? a : b;
    return float4(gv, p - gv);
}

// 六角形の「中心からの距離」。0.5 でセル境界。
float TrHexDist(float2 gv)
{
    gv = abs(gv);
    return max(dot(gv, float2(0.8660254, 0.5)), gv.y);
}

// n 芒星の「その角度での輪郭半径」。先端 = 1.0 / 谷 = inner。
// ★極座標で半径を lerp すると辺が丸く膨らんで**星ではなく花**に見える（実際そうなった）。
//   先端と谷を結ぶ「直線」との交点を解くことで、辺がまっすぐな本物の星形になる。
float TrStarRadius(float ang, float n, float inner)
{
    const float seg = TR_TAU / n;
    // 先端を 0 とする ±seg/2 のくさび形へ畳む
    const float a = fmod(ang + TR_TAU * 2.0 + seg * 0.5, seg) - seg * 0.5;

    const float2 tip   = float2(1.0, 0.0);
    const float2 valley = float2(inner * cos(seg * 0.5), inner * sin(seg * 0.5));
    const float2 edge  = valley - tip;
    const float2 nrm   = normalize(float2(edge.y, -edge.x));   // 辺の法線
    const float  c     = dot(nrm, tip);                        // 原点から辺までの距離
    const float2 u     = float2(cos(a), sin(a));
    const float  den   = dot(nrm, u);
    return (den > 1e-4) ? (c / den) : 1.0;
}

// ">" チェビロン 1 枚(シークバー早送り用)。cpos = 先端の x 位置。
float TrChevron(float2 uv, float cpos)
{
    float ay = abs(uv.y - 0.5);
    if (ay > 0.10) return 0.0;
    float edge = cpos - ay * 0.55;
    float d = abs(uv.x - edge);
    return smoothstep(0.014, 0.004, d) * smoothstep(0.10, 0.03, ay);
}

// ===== 本体 =====
// uv       … 画面 uv (0..1、v は下向き)
// progress … 被覆率 0→1→0 (閉じて開く)
// total    … 遷移全体の進行 0..1 (閉じ→開きを跨ぐ。方向や時間演出に使う型がある)
// type     … 上の対応表
// aspect   … 画面アスペクト w/h
// 戻り値   … rgb = 暗幕の色 / a = 被覆アルファ (呼び出し側で通常アルファ合成する)
float4 TransitionCurtain(float2 uv, float progress, float total, int type, float aspect)
{
    const float p    = TrPad(progress, TR_W);
    const float maxd = TrMaxRadius(aspect);

    float2 dc = uv - 0.5;
    dc.x *= aspect;

    float3 col = float3(0.0, 0.0, 0.0);
    float  a   = 0.0;

    if (type == 0)                       // fade: 暗転
    {
        a = progress;
    }
    else if (type == 5)                  // flash: ホワイトアウト
    {
        a = progress;
        col = float3(1.0, 1.0, 1.0);
    }
    else if (type == 1)                  // wipe: 横ワイプ
    {
        a = TrSoft(p - uv.x);
    }
    else if (type == 3)                  // wipe_v: 縦ワイプ
    {
        a = TrSoft(p - uv.y);
    }
    else if (type == 9)                  // wipe_diag: 斜めワイプ
    {
        a = TrSoft(p - (uv.x + uv.y) * 0.5);
    }
    else if (type == 10)                 // curtain: 左右から閉じるカーテン
    {
        float halfOpen = 0.5 * (1.0 - p);
        float d        = abs(uv.x - 0.5);
        a = TrSoft(d - halfOpen);
        // 幕の合わせ目に細い明かりを乗せる(舞台幕の縁)
        col += float3(0.45, 0.40, 0.35) * smoothstep(0.012, 0.0, abs(d - halfOpen)) * a;
    }
    else if (type == 11)                 // slide: 一枚板が右から入り左へ抜ける
    {
        // 閉じ = 右から入る / 開き = そのまま左へ抜ける ＝ 一方向のスライドに見える
        float lead = (total < 0.5) ? (1.0 - p) : p;
        a = (total < 0.5) ? TrSoft(uv.x - lead) : TrSoft(lead - uv.x);
        col += float3(0.35, 0.38, 0.45) * smoothstep(0.010, 0.0, abs(uv.x - lead)) * a;
    }
    else if (type == 6)                  // blinds: 横ブラインド
    {
        a = TrSoft(p - frac(uv.y * TR_BLIND_H));
    }
    else if (type == 12)                 // blinds_v: 縦ブラインド
    {
        a = TrSoft(p - frac(uv.x * TR_BLIND_V));
    }
    else if (type == 7)                  // clock: 時計ワイプ
    {
        float ang = atan2(dc.x, -dc.y);                       // 上が 0、時計回りが正
        float t01 = (ang < 0.0) ? (ang + TR_TAU) / TR_TAU : ang / TR_TAU;
        a = TrSoft(p - t01);
    }
    else if (type == 23)                 // spiral: 渦巻き
    {
        float ang = atan2(dc.x, -dc.y);
        float t01 = (ang < 0.0) ? (ang + TR_TAU) / TR_TAU : ang / TR_TAU;
        float r   = length(dc) / maxd;
        a = TrSoft(p - frac(t01 + r * 1.6));
        // 巻きの境目は 1 周ぶんの段差になるので、閉じ切り/開き切りは素直に倒す
        a = lerp(a, 1.0, smoothstep(0.94, 1.0, progress));
        a = lerp(0.0, a, smoothstep(0.0, 0.06, progress));
    }
    else if (type == 2)                  // iris: 円アイリス
    {
        a = TrSoft(length(dc) / maxd - (1.0 - p));
    }
    else if (type == 8)                  // diamond: 菱形アイリス
    {
        float2 ad = abs(dc);
        a = TrSoft((ad.x + ad.y) / (0.5 * aspect + 0.5) - (1.0 - p));
    }
    else if (type == 13)                 // star: 5 芒星アイリス
    {
        const float inner = 0.42;
        float r   = length(dc) / maxd;
        float ang = atan2(dc.y, dc.x) + 1.5707963;            // 先端をひとつ真上へ向ける
        // p=0 で画面（最大半径 1.0）を完全に含むには、星の最小半径 inner*scale > 1 が要る。
        float scale = (1.0 / inner) * 1.05;
        a = TrSoft(r - TrStarRadius(ang, 5.0, inner) * scale * (1.0 - p));
    }
    else if (type == 14)                 // plus: 十字アイリス
    {
        float2 ad = abs(dc);
        a = TrSoft(min(ad.x, ad.y) - 0.55 * (1.0 - p));
    }
    else if (type == 15)                 // heart: ハートアイリス
    {
        float sc = max(1.55 * (1.0 - p), 1e-4);
        float2 q = dc / sc;
        q.y = -q.y - 0.12;                                    // v は下向き＋少し下げて中心合わせ
        float hv = pow(q.x * q.x + q.y * q.y - 1.0, 3.0) - q.x * q.x * q.y * q.y * q.y;
        a = smoothstep(-0.03, 0.03, hv);                      // ハートの「外側」を覆う
    }
    else if (type == 16)                 // dissolve: ノイズディゾルブ
    {
        float n  = TrFbm(uv * float2(aspect, 1.0) * 7.0);
        float th = TrPad(progress, 0.16);
        a = 1.0 - smoothstep(th - 0.14, th + 0.14, n);
    }
    else if (type == 17)                 // mosaic: ブロックモザイク
    {
        float2 g  = float2(TR_MOSAIC, max(3.0, round(TR_MOSAIC / aspect)));
        float2 id = floor(uv * g);
        float  r  = TrHash21(id);
        // 中心から外へ広がる傾向 + ブロックごとのランダム順
        float bias = length((id + 0.5) / g - 0.5) * 0.55;
        a = step(r * 0.62 + bias * 0.55, TrPad(progress, 0.02));
        col = float3(0.015, 0.016, 0.024) * (0.5 + 1.2 * r);  // ブロックごとに濃さを散らす
    }
    else if (type == 18)                 // melt: Doom 融解
    {
        float c   = floor(uv.x * TR_MELT);
        float off = TrHash11(c * 1.731) * 0.34;
        float y   = TrPad(progress, 0.02) * 1.36 - off;
        a = TrSoft(y - uv.y);
        // 落ちてくる幕の下端に薄いハイライト(垂れている感じ)
        col += float3(0.10, 0.11, 0.14) * smoothstep(0.02, 0.0, abs(uv.y - y)) * a;
    }
    else if (type == 19)                 // shatter: ガラス割れ
    {
        float2 g   = float2(TR_SHATTER, max(3.0, round(TR_SHATTER / aspect)));
        float2 uvg = uv * g;
        float2 id  = floor(uvg);
        float2 f   = frac(uvg);
        bool   up  = (f.x + f.y) > 1.0;                       // セルを対角で 2 枚の三角に割る
        float  sid = TrHash21(id + (up ? 37.0 : 0.0));
        float  dist = length((id + 0.5) / g - 0.5);
        float  th   = TrPad(progress, 0.02);
        float  k    = saturate((th - (sid * 0.42 + dist * 0.38)) * 4.5);
        a = step(0.5, k);
        // 破片の縁(ひび)を青白く光らせる。現れる瞬間だけ強い。
        float dEdge = up ? min(min(1.0 - f.x, 1.0 - f.y), (f.x + f.y - 1.0) * 0.7071)
                         : min(min(f.x, f.y), (1.0 - f.x - f.y) * 0.7071);
        float crack = smoothstep(0.07, 0.0, dEdge) * saturate(1.0 - abs(k * 2.0 - 1.0));
        col += float3(0.55, 0.75, 1.0) * crack * 1.5;
    }
    else if (type == 20)                 // glitch: 走査帯グリッチ
    {
        float band = floor(uv.y * TR_GLITCH);
        float seed = TrHash11(band * 1.113);
        float th   = TrPad(progress, 0.02);
        a = step(seed * 0.82, th);
        float fy = frac(uv.y * TR_GLITCH);
        col  = float3(0.014, 0.011, 0.028);
        col += float3(1.00, 0.15, 0.60) * smoothstep(0.14, 0.0, fy) * 0.55;         // 上縁マゼンタ
        col += float3(0.15, 0.90, 1.00) * smoothstep(0.14, 0.0, 1.0 - fy) * 0.55;   // 下縁シアン
        col *= 0.78 + 0.22 * step(0.5, frac(uv.y * 220.0));                          // 走査線
        // 出現しかけの帯だけ横に飛ぶ(テープのずれ)
        float jitter = TrHash11(band * 3.1 + floor(total * 26.0)) - 0.5;
        float edge   = saturate(1.0 - abs((th - seed * 0.82) * 9.0));
        a *= 1.0 - edge * step(0.5, frac(uv.x * 3.0 + jitter * 4.0)) * 0.35;
    }
    else if (type == 21)                 // hex: ハニカム
    {
        float4 h    = TrHexCell(uv * float2(aspect, 1.0) * TR_HEX);
        float  r    = TrHash21(h.zw);
        float  dist = length(dc) / maxd;
        float  th   = TrPad(progress, 0.02);
        float  k    = saturate((th - (dist * 0.42 + r * 0.22)) / 0.34);
        float  hd   = TrHexDist(h.xy);
        // 0.78 > 六角形の角までの距離＝完全に閉じたときセル同士が重なって隙間が残らない
        a = TrSoft(k * 0.78 - hd);
        col += float3(0.22, 0.50, 0.66) * smoothstep(0.075, 0.0, abs(k * 0.78 - hd)) * a * 1.4;
    }
    else if (type == 22)                 // checker: 市松
    {
        float2 g   = float2(TR_CHECKER, max(3.0, round(TR_CHECKER / aspect)));
        float2 id  = floor(uv * g);
        float  par = fmod(id.x + id.y, 2.0);                  // 0 = 先攻マス / 1 = 後攻マス
        float  th  = TrPad(progress, 0.02);
        float  k   = saturate((th - par * 0.42) / 0.58);
        float2 f   = abs(frac(uv * g) - 0.5);
        a = TrSoft(k * 0.78 - max(f.x, f.y));
    }
    else if (type == 24)                 // ripple: 波紋
    {
        float r   = length(dc) / maxd;
        float th  = TrPad(progress, 0.02);
        float wob = sin(r * 34.0 - total * 14.0) * 0.014;     // 縁が波打つ
        float w   = r - th * 1.16 + wob;
        a = TrSoft(-w);
        col  = float3(0.010, 0.030, 0.055);
        col += float3(0.35, 0.72, 1.00) * exp(-abs(w) * 30.0) * 1.1;
    }
    else if (type == 25)                 // flood: 水没
    {
        float level = TrPad(progress, 0.05);
        float wave  = sin(uv.x * 18.0 + total * 9.0) * 0.013
                    + sin(uv.x * 31.0 - total * 6.0) * 0.007;
        float surf  = (1.0 - level) + wave;                   // 画面上の水面 y
        a = TrSoft(uv.y - surf);
        col  = float3(0.014, 0.055, 0.095);
        col += float3(0.30, 0.70, 0.92) * exp(-abs(uv.y - surf) * 80.0) * 1.3;
        col += float3(0.05, 0.12, 0.18) * saturate((uv.y - surf) * 2.0);   // 深いほど濃い
    }
    else if (type == 26)                 // burn: フィルムバーン
    {
        float n  = TrFbm(uv * float2(aspect, 1.0) * 4.5) * 0.70 + uv.x * 0.30;
        float th = TrPad(progress, 0.18);
        float d  = n - th;                                    // >0 = まだ焼けていない
        a = TrSoft(-d);
        col  = float3(0.020, 0.014, 0.010);
        col += float3(1.00, 0.34, 0.05) * smoothstep(0.10, 0.0, abs(d)) * 1.7;
        col += float3(1.00, 0.86, 0.35) * smoothstep(0.030, 0.0, abs(d)) * 2.4;
        a = max(a, smoothstep(0.045, 0.0, d) * 0.92);         // 焼け縁は未焼け側へも滲む
    }
    else if (type == 27)                 // rush: 集中線
    {
        float ang = atan2(dc.y, dc.x);
        float r   = length(dc) / maxd;
        float th  = TrPad(progress, 0.02);

        float s  = (ang / TR_TAU + 0.5) * TR_RUSH;
        float ai = floor(s);
        float af = frac(s);
        float w  = 0.16 + TrHash11(ai * 1.37) * 0.42;                    // 線の太さ
        // ★線は暗転より「速く」内側へ伸ばすこと。逆にすると黒が先に画面を覆って
        //   集中線が一度も見えないまま終わる（最初そうなっていた）。
        float inner = 1.0 - th * 1.70 - TrHash11(ai * 2.11) * 0.30;      // 線の内側の先端

        float black = TrSoft(r - (1.0 - th * 1.06));                     // 外周から閉じる暗転
        float white = TrSoft(w * 0.5 - abs(af - 0.5)) * TrSoft(r - inner)
                    * (1.0 - black) * 0.9;

        a = saturate(black + white);
        col = (a > 1e-4) ? (float3(1.0, 1.0, 1.0) * white) / a : float3(0.0, 0.0, 0.0);
    }
    else if (type == 4)                  // seek: シークバー早送り
    {
        // 光るプレイヘッドが左→右へ 2 回掃く(閉じで覆い、開きで新シーンを開放)。
        // 画面下に動画プレイヤー風シークバーが全体進行と同期して伸びる。
        const float3 gold   = float3(1.0, 0.62, 0.15);
        const bool   closing = (total < 0.5);
        float front = closing ? progress : (1.0 - progress);
        float x     = uv.x + (uv.y - 0.5) * 0.10;                        // エッジは少し斜め(疾走感)

        float covered = closing ? smoothstep(front + 0.002, front - 0.004, x)
                                : smoothstep(front - 0.002, front + 0.004, x);
        col  = float3(0.016, 0.020, 0.048);
        col += 0.015 * step(0.5, frac(uv.y * 90.0));
        a = covered;

        float behind = closing ? (front - x) : (x - front);
        float glow   = (behind > 0.0) ? exp(-behind * 26.0) : 0.0;
        col += gold * glow * 1.6;

        float dirSign = closing ? 1.0 : -1.0;
        float chev = TrChevron(uv, front - dirSign * 0.050)
                   + TrChevron(uv, front - dirSign * 0.095) * 0.6;
        col += gold * chev * covered * 1.2;

        float barY  = 0.935;
        float inBar = smoothstep(0.0045, 0.0035, abs(uv.y - barY));
        if (inBar > 0.0)
        {
            float  fill   = smoothstep(total + 0.002, total - 0.002, uv.x);
            float3 barCol = lerp(float3(0.35, 0.35, 0.38), gold, fill);
            col = lerp(col, barCol, inBar);
            a   = max(a, inBar * 0.85);
        }
        float2 kd = uv - float2(total, barY);
        kd.x *= aspect;
        float knob = smoothstep(0.009, 0.006, length(kd));
        if (knob > 0.0)
        {
            col = lerp(col, gold * 1.4, knob);
            a   = max(a, knob);
        }
    }
    else                                 // 未知の値のフォールバック = 暗転
    {
        a = progress;
    }

    return float4(col, saturate(a));
}

#endif // DX12E_TRANSITION_CURTAIN_HLSLI
