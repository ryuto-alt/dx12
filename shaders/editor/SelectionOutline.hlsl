// SelectionOutline.hlsl — エディタ専用の選択 / ホバーの輪郭（フェーズ 1a）。
//
// ★ゲーム・Play・screenshot_final には一切影響しない: バックバッファへ「ポスト後・ImGui の直前・
//   最終画の撮影より後」に足すだけで、C++ 側（SelectionOutlinePass）はエディタモードのときだけ呼ぶ。
// ★メインのルートシグネチャには触れない。専用ルートシグネチャ（38 DWORD）:
//     [0] b0 32bit 定数 20 … マスク描画: transpose(world) 16 + (selected, hover, 0, 0)
//                            合成      : 選択色 4 + ホバー色 4 + 縁の太さ等 4 + マスク原点 / サイズ 4 + 予備 4
//     [1] b1 32bit 定数 16 … transpose(viewProj)（マスク描画だけ）
//     [2] テーブル t3      … ボーン行列（スキンドの VS だけ）
//     [3] テーブル t0      … マスク（合成の PS）
//
// 1) マスク描画（R8G8_UNORM）: 選択の物を R=1、ホバーの物を G=1 で塗る（深度なし・両面）。
//    ブレンドは MAX なので重なっても 1 のまま。
// 2) 合成: マスクの外側の画素について、最も近いマスク画素までの距離 d を探索し、
//    芯（くっきりした線）+ 外側へ 2 乗で減衰する光を「アクセント色 × 縁の濃さ」でバックバッファへ重ねる。
//    式は src/editor/ViewportLogic.h の outline::EdgeAlpha と同じ（単体テストで検算している）。

// ---------------------------------------------------------------- マスク描画
cbuffer PerDraw : register(b0)
{
    float4x4 model;      // transpose(world)（行ベクトル規約で mul(v, model)）
    float4   maskValue;  // x = 選択, y = ホバー
};

cbuffer PerPass : register(b1)
{
    float4x4 viewProj;   // transpose(viewProj)
};

StructuredBuffer<float4x4> g_bones : register(t3);

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float2 texCoord    : TEXCOORD0;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

float4 VSMask(VSInput input) : SV_POSITION
{
    const float4 wp = mul(float4(input.position, 1.0f), model);
    return mul(wp, viewProj);
}

float4 VSMaskSkinned(VSInput input) : SV_POSITION
{
    float4x4 skinMatrix =
        input.boneWeights.x * g_bones[input.boneIndices.x] +
        input.boneWeights.y * g_bones[input.boneIndices.y] +
        input.boneWeights.z * g_bones[input.boneIndices.z] +
        input.boneWeights.w * g_bones[input.boneIndices.w];
    const float4 sp = mul(float4(input.position, 1.0f), skinMatrix);
    const float4 wp = mul(sp, model);
    return mul(wp, viewProj);
}

float4 PSMask(float4 pos : SV_POSITION) : SV_Target0
{
    return float4(maskValue.x, maskValue.y, 0.0f, 0.0f);
}

// ワイヤ表示モード（ビューモード「ワイヤ」）: 同じ VS で FILL_MODE_WIREFRAME の PSO から描く。色は b0 の maskValue（rgba）。
float4 PSWire(float4 pos : SV_POSITION) : SV_Target0
{
    return maskValue;
}

// ---------------------------------------------------------------- 合成
cbuffer Composite : register(b0)
{
    float4 selColor;     // rgb = 選択の輪郭色（sRGB のままバックバッファへ）/ a = 全体の強さ
    float4 hoverColor;   // rgb = ホバーの輪郭色 / a = 全体の強さ
    float4 edge;         // x = 芯の太さ(px) y = 光の広がり(px) z = 光の強さ w = 探索半径(px, 整数)
    float4 maskRect;     // x,y = マスク原点（バックバッファ座標）z,w = マスクの幅・高さ
    float4 spare;
};

Texture2D<float2> g_mask : register(t0);

struct VSOut
{
    float4 pos : SV_POSITION;
};

// 画面全体の三角形（頂点バッファなし）
VSOut VSFull(uint id : SV_VertexID)
{
    VSOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

// ワイヤ表示モードの下地（ビューポート矩形を単色で塗る）。色は Composite の selColor。
float4 PSFill(VSOut i) : SV_Target0
{
    return selColor;
}

// 距離 d（マスク外）→ 縁の濃さ。ViewportLogic.h の outline::EdgeAlpha と同じ式。
float EdgeAlpha(float d, float corePx, float glowPx, float glowStrength)
{
    if (d <= 0.0f) return 0.0f;
    const float core = saturate(corePx + 0.5f - (d - 0.5f));
    const float g = saturate(1.0f - (d - 0.5f) / max(glowPx, 1e-3f));
    return max(core, glowStrength * g * g);
}

float4 PSComposite(VSOut i) : SV_Target0
{
    const int2 p = int2(i.pos.xy) - int2(maskRect.xy);
    const int2 sz = int2(maskRect.zw);
    if (p.x < 0 || p.y < 0 || p.x >= sz.x || p.y >= sz.y) discard;

    const float2 self = g_mask.Load(int3(p, 0));
    const int R = (int)edge.w;

    // 選択（R）とホバー（G）それぞれの「最も近いマスク画素までの距離」
    float dSel = R + 1.0f;
    float dHov = R + 1.0f;
    const bool selIn = self.x > 0.5f;
    const bool hovIn = self.y > 0.5f;
    for (int dy = -R; dy <= R; ++dy)
    {
        for (int dx = -R; dx <= R; ++dx)
        {
            const int2 q = p + int2(dx, dy);
            if (q.x < 0 || q.y < 0 || q.x >= sz.x || q.y >= sz.y) continue;
            const float2 m = g_mask.Load(int3(q, 0));
            const float d = sqrt((float)(dx * dx + dy * dy));
            if (m.x > 0.5f) dSel = min(dSel, d);
            if (m.y > 0.5f) dHov = min(dHov, d);
        }
    }
    // 選択の物の内側は塗らない（内側 = 物そのものの絵を見せる）。ホバーも同様。
    const float aSel = selIn ? 0.0f : EdgeAlpha(dSel, edge.x, edge.y, edge.z) * selColor.a;
    // ホバーは細く弱く（芯だけ・光なし）。選択と重なる画素は選択を優先する。
    const float aHov = (hovIn || aSel > 0.0f) ? 0.0f : EdgeAlpha(dHov, edge.x * 0.75f, 1.0f, 0.0f) * hoverColor.a;

    const float a = aSel + aHov;
    if (a <= 0.001f) discard;
    const float3 rgb = (aSel * selColor.rgb + aHov * hoverColor.rgb) / a;
    return float4(rgb, saturate(a));
}
