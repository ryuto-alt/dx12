// Particle.hlsl - GPUインスタンシング加算ビルボードパーティクル
//   頂点バッファ無し: SV_VertexID で四角形を生成、インスタンスストリームで粒子情報を受ける。
//   グローはテクスチャ不要 ── PS で UV から解析的に放射状フォールオフを計算する。
//   HDR(RGBA16F)へ加算ブレンドで描き、ポスト側のブルーム＋トーンマップで白熱して光る。

cbuffer CamCB : register(b0)
{
    float4x4 viewProj;   // 行ベクトル前提 (mul(rowvec, M))。CPU側で転置して渡す
    float4   camRight;   // ワールド空間カメラ右ベクトル (xyz)
    float4   camUp;      // ワールド空間カメラ上ベクトル (xyz)
    float4   params;     // x=globalIntensity, y=softness, z/w=予約
};

struct VSInput
{
    float3 center  : POSITION;    // ワールド中心
    float  size    : TEXCOORD0;   // 半径(ワールド)
    float4 color   : COLOR0;      // rgb=HDR色, a=アルファ
    float  rot     : TEXCOORD1;   // 回転(ラジアン)
    float  stretch : TEXCOORD2;   // >0 で速度方向へ伸ばす
    float3 vel     : NORMAL0;     // ワールド速度（ストレッチ用）
    uint   vid     : SV_VertexID;
};

struct VSOutput
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    float2 uv    : TEXCOORD0;    // [-1,1] 中心原点
};

// 四角形2三角形分のコーナー
static const float2 kCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1),
    float2(-1, -1), float2(1,  1), float2(-1, 1)
};

VSOutput VSMain(VSInput i)
{
    float2 c = kCorners[i.vid];
    float3 worldPos;

    if (i.stretch > 0.0 && dot(i.vel, i.vel) > 1e-4)
    {
        // 速度方向に伸びるストレッチビルボード（火花/筋/弾道）。
        float3 vd = normalize(i.vel);
        float3 axisX = cross(vd, camRight.xyz);
        if (dot(axisX, axisX) < 1e-5) axisX = camUp.xyz;
        axisX = normalize(axisX);
        float speedMag = length(i.vel);
        float halfW = i.size * 0.55;
        float halfH = i.size * (1.0 + i.stretch * min(speedMag, 30.0) * 0.06);
        worldPos = i.center + axisX * (c.x * halfW) + vd * (c.y * halfH);
    }
    else
    {
        // 通常の丸ビルボード（回転は火花の向き等に使用）
        float s = sin(i.rot), co = cos(i.rot);
        float2 r = float2(c.x * co - c.y * s, c.x * s + c.y * co);
        worldPos = i.center
                 + camRight.xyz * (r.x * i.size)
                 + camUp.xyz    * (r.y * i.size);
    }

    VSOutput o;
    o.pos   = mul(float4(worldPos, 1.0), viewProj);
    o.color = i.color;
    o.uv    = c;   // フォールオフは放射状なので未回転の c を使う
    return o;
}

float4 PSMain(VSOutput i) : SV_TARGET
{
    float r2   = dot(i.uv, i.uv);           // 中心0 .. 辺1 .. 角2
    float fall = saturate(1.0 - r2);        // 円形ソフトディスク
    fall = pow(fall, params.y);             // コアを締める

    float a   = i.color.a * fall;
    float3 c  = i.color.rgb * a * params.x; // 事前乗算 + HDR増幅（加算ブレンド前提）
    return float4(c, a);
}
