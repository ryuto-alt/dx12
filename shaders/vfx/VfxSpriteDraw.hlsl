// VfxSpriteDraw.hlsl - UnoFX フリップブックの描画（W1-BD7 / VFX-03）。VfxDraw.hlsl（円形の既定スプライト）の flipbook 版。
//   * Sub-UV（枠の切り替え）、MV 補間（引く位置で 2 回評価し直し、枠の矩形の半テクセル内側へクランプ）、6-way ライティング、黒体の色、α の浸食。
//   * 根元固定（anchor = bottom）: 四角形の下端を粒子の位置に合わせる。炎と煙の枠は下端に根元がある（NB2 の規約）。
//   * 出力は premultiplied（additive は alpha 0、alpha ブレンドは alpha > 0）。PSO は VfxDraw と同じ ONE / INV_SRC_ALPHA 1 本。
//   * 環境光は灰色のスカラー（フレーム定数の ambientStrength * iblIntensity。IBL の色味は使わない仮置き）で始める。W2 の VFX-05（TLV）で置き換える。
// ルートシグネチャ: b0 = DrawCB（36 DWORD。VfxDraw と同じ）, t0 = gPool（root SRV, VS）, b1 = FlipCB（24 DWORD）, s0 = 線形クランプ（静的）
#define UNO_VFX_NO_OPS
#include "VfxCommon.hlsli"
#include "VfxSixWay.hlsli"

cbuffer DrawCB : register(b0)
{
    float4x4 viewProj;   // 転置済み（mul(rowvec, M)）
    float4   camRight;
    float4   camUp;
    float4   params;     // x = intensity, y = softDistance, z = fade（0 none, 1 linear, 2 quad, 3 inout）, w = stretch
    float4   params2;    // x = projA, y = projB, z = 1/W, w = 1/H
    uint4    uP;         // x = base, y = 深度 SRV 番号, z = bit0: 0（additive）/ 1（alpha）, bit1: 正射, w = 1 なら実深度あり
};

cbuffer FlipCB : register(b1)
{
    uint4  fTex;    // x = 色（炎族 / RLU）の SRV, y = DBF の SRV, z = MV の SRV（0xFFFFFFFF で無し）, w = 下位 4bit: チャンネル（0 炎族, 1 6-way, 2 色）、bit4: 根元固定
    uint4  fGrid;   // x = cols, y = rows, z = frames, w = rootFade（float のビット）
    float4 fP0;     // x = fps, y = mvRange, z = erodeStart, w = erodeWidth
    float4 fLight;  // xyz = 表面から光へ（ワールド）, w = tempMin
    float4 fSun;    // rgb = 光の色, w = tempMax
    float4 fAmb;    // rgb = 環境光, w = 放射の倍率
};

StructuredBuffer<GPart> gPool : register(t0);
SamplerState gLinearClamp : register(s0);

struct VSOut
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;                       // 色（intensity 込み）と alpha（fade 込み）
    float2 uv    : TEXCOORD0;                    // 枠内 uv（左上 (0,0)、v 下向き）
    float  viewZ : TEXCOORD1;
    nointerpolation float  tLife : TEXCOORD2;
    nointerpolation float3 lightB : TEXCOORD3;   // ビルボード空間の光の向き
    nointerpolation uint2  frames : TEXCOORD4;   // a, b
    nointerpolation float  ft     : TEXCOORD5;   // 補間の t
};

static const float2 kCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1),
    float2(-1, -1), float2(1,  1), float2(-1, 1)
};

float FadeCurve(float t, uint mode)
{
    if (mode == 1) return 1.0 - t;
    if (mode == 2) return (1.0 - t) * (1.0 - t);
    if (mode == 3) return smoothstep(0.0, 0.1, t) * (1.0 - t);
    return 1.0;
}

VSOut VSMain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    GPart p = gPool[uP.x + iid];
    VSOut o;
    o.color = 0; o.uv = 0; o.viewZ = 0; o.tLife = 0; o.lightB = 0; o.frames = 0; o.ft = 0;
    if (p.life <= 0.0 || p.age >= p.life)
    {
        o.pos = float4(2.0, 2.0, 2.0, 1.0);   // クリップ外（四角形ごと捨てる）
        return o;
    }
    float2 c = kCorners[vid];
    float t = saturate(p.age / p.life);
    float2 sz = UnpackHalf2(p.size);
    float size = lerp(sz.x, sz.y, t);
    float4 col = lerp(UnpackColor(p.c0a, p.c0b), UnpackColor(p.c1a, p.c1b), t);
    float2 rot = UnpackHalf2(p.rot);
    float angle = rot.x;   // VfxDraw と同じ（角速度は使わない）

    float3 right = camRight.xyz, up = camUp.xyz;
    float3 towardCam = -cross(right, up);   // ビュー空間 +z（奥）の反対 = カメラ側
    float3 wp;
    bool anchorBottom = (fTex.w & 16u) != 0u;
    float3 bx = right, by = up, bz = towardCam;   // 四角形の実際の軸（6-way の光をこの軸で表す）
    if (params.w > 0.0 && dot(p.vel, p.vel) > 1e-4)
    {
        // 速度方向へ伸ばす（火花）。伸ばすときは中心基準
        // 画像の +u（c.x）= 速度方向（火花の頭）、v = 直交方向（NB2 の sparks.dds の規約）。VfxFlipbook.h の StretchOffset と同じ
        float3 vd = normalize(p.vel);
        // 幅の軸 = vd × 視線。四角形の法線 (vd·T)vd - T がカメラ側（T）に向き、どの速度でも画面に出る（camRight との外積だと、真上へ昇る火花などで真横から見る向きになり消える）
        float3 axisX = cross(vd, towardCam);
        if (dot(axisX, axisX) < 1e-5) axisX = right;   // 速度がほぼ視線方向
        axisX = normalize(axisX);
        bx = vd; by = axisX;
        bz = cross(bx, by); if (dot(bz, towardCam) < 0.0) bz = -bz;
        // 注意: ここで bz を反転すると基底の手系が反転し、6-way の左右の光が鏡映しになる。伸ばす描画に smoke6way を付けるときは要見直し（今は炎族のみ）
        float stretch = 1.0 + params.w * min(length(p.vel), 30.0) * 0.06;
        wp = p.pos + (vd * stretch) * (c.x * size) + (axisX * 0.55) * (c.y * size);
        angle = 0.0;
    }
    else
    {
        float2 q = c;
        if (anchorBottom) q.y += 1.0;   // 下端（c.y = -1）が粒子の位置。回転は根元まわり
        float s = sin(angle), cs = cos(angle);
        q = float2(q.x * cs - q.y * s, q.x * s + q.y * cs);
        wp = p.pos + right * (q.x * size) + up * (q.y * size);
    }
    o.pos = mul(float4(wp, 1.0), viewProj);
    o.color = float4(col.rgb * params.x, col.a * FadeCurve(t, (uint)(params.z + 0.5)));
    o.uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    o.viewZ = o.pos.w;
    o.tLife = t;

    float3 Lw = fLight.xyz;
    float cx = dot(Lw, bx), cy = dot(Lw, by), cz = dot(Lw, bz);
    float sa = sin(angle), ca = cos(angle);
    o.lightB = float3(cx * ca + cy * sa, -cx * sa + cy * ca, cz);

    FbPick pick = FbPickFrames(fGrid.z, p.age, p.life, fP0.x, FbStartFrame(p.id, fGrid.z));
    o.frames = uint2(pick.a, pick.b);
    o.ft = pick.t;
    return o;
}

float RootFadeV(float v, float fade)
{
    if (fade <= 0.0) return 1.0;
    return smoothstep(0.0, fade, 1.0 - v);   // VfxFlipbook.h の RootFade と同じ
}

float4 PSMain(VSOut i) : SV_Target
{
    uint2 grid = fGrid.xy;
    float rootFade = asfloat(fGrid.w);   // 根元のフェード（枠の高さに対する割合。0 で無し）
    uint channel = fTex.w & 15u;
    float2 pa = i.uv, pb = i.uv;
    if (fTex.z != 0xFFFFFFFFu)
    {
        Texture2D<float2> mv = ResourceDescriptorHeap[fTex.z];
        FbResolveTaps(mv, gLinearClamp, grid, fP0.y, i.uv, i.ft, i.frames.x, i.frames.y, pa, pb);
    }
    Texture2D<float4> tex0 = ResourceDescriptorHeap[fTex.x];
    float4 s0 = lerp(FbSampleFrame(tex0, gLinearClamp, grid, i.frames.x, pa),
                     FbSampleFrame(tex0, gLinearClamp, grid, i.frames.y, pb), i.ft);
    FbShaded sh;
    if (channel == 0u)
    {
        sh = FbShadeFire(s0, i.tLife, float3(fLight.w, fSun.w, fAmb.w), fP0.zw);
        sh.rgb *= i.color.rgb;
    }
    else if (channel == 1u)
    {
        Texture2D<float4> tex1 = ResourceDescriptorHeap[fTex.y];
        float4 s1 = lerp(FbSampleFrame(tex1, gLinearClamp, grid, i.frames.x, pa),
                         FbSampleFrame(tex1, gLinearClamp, grid, i.frames.y, pb), i.ft);
        sh = FbShadeSmoke(s0, s1, normalize(i.lightB), i.tLife, i.color.rgb, fAmb.rgb, fSun.rgb, fP0.zw);
    }
    else
    {
        sh.rgb = s0.rgb * i.color.rgb;
        sh.a = s0.a;
    }
    float a = i.color.a * sh.a * RootFadeV(i.uv.y, rootFade);

    if (uP.w != 0u && uP.y != 0xFFFFFFFFu)
    {
        Texture2D<float> depth = ResourceDescriptorHeap[uP.y];
        float d = depth.Load(int3(int2(i.pos.xy), 0));
        if (d < 1.0)
        {
            // ハードウェアの深度テストの代わり（VfxDraw と同じ）
            if (i.pos.z > d) discard;
            if (params.y > 0.0)
            {
                bool ortho = (uP.z & 2u) != 0u;
                float den = d - params2.x;
                if (ortho)
                {
                    if (abs(params2.x) > 1e-12)
                    {
                        float sceneZ = (d - params2.y) / params2.x;
                        float selfZ  = (i.pos.z - params2.y) / params2.x;
                        a *= saturate((sceneZ - selfZ) / params.y);
                    }
                }
                else if (abs(den) > 1e-8)
                {
                    float sceneZ = params2.y / den;
                    a *= saturate((sceneZ - i.viewZ) / params.y);
                }
            }
        }
    }
    return float4(sh.rgb * a, (uP.z & 1u) == 0u ? 0.0 : a);
}
