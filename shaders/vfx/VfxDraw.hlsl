// VfxDraw.hlsl - UnoFX の描画（W1-BD4）。スプライト（カメラ向きの四角形。flipbook は BD7 で足す）。
//   頂点バッファ無し: SV_VertexID で四角形、SV_InstanceID でリング内の slot を引く。死んだ粒子は画面外へ出す。
//   出力は premultiplied（additive は alpha 0、alpha ブレンドは alpha > 0）。PSO は ONE / INV_SRC_ALPHA 1 本。
//   深度は SM 6.6 の ResourceDescriptorHeap[] で引く（無いビューではダミー SRV を渡し、softDistance = 0 で読まない）。
// ルートシグネチャ: b0 = DrawCB（36 DWORD）, t0 = gPool（root SRV, VS）
#define UNO_VFX_NO_OPS
#include "VfxCommon.hlsli"

cbuffer DrawCB : register(b0)
{
    float4x4 viewProj;   // 転置済み（mul(rowvec, M)）
    float4   camRight;
    float4   camUp;
    float4   params;     // x = intensity, y = softDistance, z = fade（0 none, 1 linear, 2 quad, 3 inout）, w = stretch
    float4   params2;    // x = projA, y = projB, z = 1/W, w = 1/H
    uint4    uP;         // x = base, y = 深度 SRV 番号, z = 0（additive）/ 1（alpha）, w = 予約
};

StructuredBuffer<GPart> gPool : register(t0);

struct VSOut
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;     // premultiplied 前の rgb と alpha（PS で掛ける）
    float2 uv    : TEXCOORD0;
    float  viewZ : TEXCOORD1;
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
    if (p.life <= 0.0 || p.age >= p.life)
    {
        o.pos = float4(2.0, 2.0, 2.0, 1.0);   // クリップ外（四角形ごと捨てる）
        o.color = 0; o.uv = 0; o.viewZ = 0;
        return o;
    }
    float2 c = kCorners[vid];
    float t = saturate(p.age / p.life);
    float2 sz = UnpackHalf2(p.size);
    float size = lerp(sz.x, sz.y, t);
    float4 col = lerp(UnpackColor(p.c0a, p.c0b), UnpackColor(p.c1a, p.c1b), t);
    float2 rot = UnpackHalf2(p.rot);

    float3 right = camRight.xyz, up = camUp.xyz;
    float2 q = c;
    if (params.w > 0.0 && dot(p.vel, p.vel) > 1e-4)
    {
        // 速度方向へ伸ばす（火花）
        float3 vd = normalize(p.vel);
        float3 axisX = cross(vd, camRight.xyz);
        if (dot(axisX, axisX) < 1e-5) axisX = camUp.xyz;
        axisX = normalize(axisX);
        float stretch = 1.0 + params.w * min(length(p.vel), 30.0) * 0.06;
        right = axisX * 0.55; up = vd * stretch;
    }
    else
    {
        float s = sin(rot.x), cs = cos(rot.x);
        q = float2(c.x * cs - c.y * s, c.x * s + c.y * cs);
    }
    float3 wp = p.pos + right * (q.x * size) + up * (q.y * size);
    o.pos = mul(float4(wp, 1.0), viewProj);
    o.color = float4(col.rgb * params.x, col.a * FadeCurve(t, (uint)(params.z + 0.5)));
    o.uv = c;
    o.viewZ = o.pos.w;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float r2 = dot(i.uv, i.uv);
    if (r2 >= 1.0) discard;
    float shape = (1.0 - r2);
    shape *= shape;
    float a = i.color.a * shape;
    if (params.y > 0.0 && uP.y != 0xFFFFFFFFu)
    {
        Texture2D<float> depth = ResourceDescriptorHeap[uP.y];
        float d = depth.Load(int3(int2(i.pos.xy), 0));
        float den = d - params2.x;   // 標準 Z: d = A + B / z（A = proj._33, B = proj._43 < 0）→ 分母は負。符号を保ったまま割る
        if (d < 1.0 && abs(den) > 1e-8)
        {
            float sceneZ = params2.y / den;   // 線形ビュー深度
            a *= saturate((sceneZ - i.viewZ) / params.y);
        }
    }
    float3 rgb = i.color.rgb * a;
    return float4(rgb, uP.z == 0 ? 0.0 : a);
}
