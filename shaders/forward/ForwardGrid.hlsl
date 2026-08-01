// ForwardGrid.hlsl - 手続き的グリッド床（エディタの足場グリッド）
//
// ★ワールド座標は「頂点属性の補間」ではなく「ピクセルごとのレイ ⇔ 平面 交差」で求める。
//   グリッド板は ±10km(kEditorGridSize) の 1 枚のクアッド＝三角形 2 枚。この巨大三角形では
//   ラスタライザの属性補間の精度が足りず、原点付近で 1〜2m ずれる。実害は 2 つ:
//     ・三角形の対角線を境に格子と赤/青の軸線がガクッと折れる（対角線が視線と直交する
//       角度＝ちょうど斜め 45 度から見たときに、画面を横切る折れ線として出る）
//     ・ddx/ddy が暴れて LOD(level) が 2x2 クアッドごとに揺れる
//       ＝視点を動かすと線がチラつく／消える
//   ★実測: worldPos を渡しても clipPos(=SV_POSITION と同値) を渡しても【同じ折れが出る】。
//     つまり「何を渡すか」の問題ではなく、この三角形で補間器を使うこと自体が駄目。
//     PS で信用できるのは補間器を通らない SV_POSITION（ラスタライザ直産のピクセル座標）だけ。
//   そこから view/proj で視線を復元し、板の平面(y = model._42)と交差させる。板の大きさにも
//   カメラのワールド位置にも精度が依存しなくなる。ついでに座標を「カメラからの相対 XZ」で
//   持てるので ddx/ddy も無誤差になる。

// PerFrame 定数バッファ(b1) は Lighting.hlsli を include して共有定義を使う。
// これで C++ FrameConstants とのバイト一致が 1 箇所で担保され、二重宣言のドリフトが起きない。
#include "Lighting.hlsli"

// Texture and sampler (unused, but RootSignature requires binding)
Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

// PerObject constants (b0) - MVP + Model matrix as RootConstants (32 DWORD)
cbuffer PerObjectConstants : register(b0)
{
    float4x4 mvp;
    float4x4 model;
};

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 color       : COLOR;
    float2 texCoord    : TEXCOORD0;
    float4 tangent     : TANGENT;
    uint4  boneIndices : BLENDINDICES;
    float4 boneWeights : BLENDWEIGHT;
};

// ★PS へ渡す補間値は 1 つも無い。巨大三角形では【どんな頂点属性も】ラスタライザの
//   補間精度が足りない（worldPos でも clipPos でも同じ折れが出る）。SV_POSITION だけは
//   補間ではなくラスタライザが直接生成するピクセル座標なので唯一信用できる。
struct PSInput
{
    float4 positionSV : SV_POSITION;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.positionSV = mul(float4(input.position, 1.0f), mvp);
    return output;
}

// ---------------------------------------------------------------------------
// アンチエイリアス済みの格子線の被覆率(0..1)。uv/uvDeriv はセル数単位、widthPx は画面px。
// 手順は Ben Golus "The Best Darn Grid Shader (Yet)" に合わせてある:
//   ・drawWidth を 1px 未満にしない。細るぶんは輝度で表す(phone-wire AA)＝
//     遠距離/低角度でも線が「消える」のではなく「薄くなる」だけになる
//   ・1 セルがサブピクセルまで詰まったら一様な塗りへ落としてモアレを止める
// ponytail: 線幅が 0.5 セルを超える極太ケースの反転処理は使わないので省略。
// ---------------------------------------------------------------------------
float GridCoverage(float2 uv, float2 uvDeriv, float widthPx)
{
    float2 target  = widthPx * uvDeriv;                    // 線幅(セル比)
    float2 drawW   = clamp(target, uvDeriv, 0.5f);
    float2 lineAA  = uvDeriv * 1.5f;
    float2 gridUV  = 1.0f - abs(frac(uv) * 2.0f - 1.0f);   // 0=線上, 1=セル中央
    float2 g       = smoothstep(drawW + lineAA, drawW - lineAA, gridUV);
    g             *= saturate(target / drawW);             // 1px 未満ぶんを輝度で補償
    g              = lerp(g, target, saturate(uvDeriv * 2.0f - 1.0f));  // モアレ抑制
    return lerp(g.x, 1.0f, g.y);
}

// 軸線(1 本だけ)の被覆率。格子と違い繰り返さないのでモアレ対策は要らない。
float AxisCoverage(float coord, float deriv, float widthPx)
{
    float px = abs(coord) / deriv;   // 軸までの距離(ピクセル)
    return 1.0f - smoothstep(widthPx - 0.5f, widthPx + 0.5f, px);
}

float4 PSMain(PSInput input) : SV_TARGET
{
    // ---- ピクセルの視線と床平面の交差 ----
    // ビューポート UV → NDC。clusterViewport.zw = (CLUSTER_GRID_X/vpW, CLUSTER_GRID_Y/vpH)
    // で、SV_Position.xy は RT 座標かつビューポート原点は常に 0（Lighting.hlsli 参照）。
    // ＝これがシェーダーからビューポート px サイズを知る既存の唯一の口。
    float2 uv01 = input.positionSV.xy * clusterViewport.zw
                / float2(CLUSTER_GRID_X, CLUSTER_GRID_Y);
    float2 ndc  = float2(uv01.x * 2.0f - 1.0f, 1.0f - uv01.y * 2.0f);
    // NDC → view 空間の方向 → ワールド方向。proj._31/_32 は TAA ジッタぶんの平行移動
    // （現状 0 だが引いておけばジッタを入れても線が半ピクセルずれない）。
    float3 dirView = float3((ndc.x - proj._31) / proj._11,
                            (ndc.y - proj._32) / proj._22,
                            1.0f);
    // view は行ベクトル規約(world→view)。3x3 は直交なので mul(M, v) が逆回転になる。
    // ponytail: 板は常に軸平行(GridPlane はエディタ内部生成で回転しない)前提。
    //           回転させたいときはここを平面法線での交差に置き換える。
    float3 dir = mul((float3x3)view, dirView);
    if (abs(dir.y) < 1e-8f) return 0.0f;                 // 地平線と平行＝床に当たらない
    float t = (model._42 - cameraPos.y) / dir.y;
    if (t <= 0.0f) return 0.0f;                          // カメラの後ろ側

    float2 local = dir.xz * t;   // カメラからの相対 XZ。小さい値なので ddx/ddy が無誤差

    // ワールド 1 単位あたりの画面変化量。ddx/ddy をベクトルとして長さを取る。
    // fwidth(=|ddx|+|ddy|) より斜め視点で正確＝カメラを回しても線幅がぶれない。
    float2 deriv = max(float2(length(float2(ddx(local.x), ddy(local.x))),
                              length(float2(ddx(local.y), ddy(local.y)))), 1e-6f);

    // セル間隔を 10 倍刻みで自動切替する（Blender/Unity と同じ考え方）。
    // 1セルが画面上で kTargetPx を切らないよう桁を上げる ＝ 遠くでも高空でも
    // 格子が詰まってモアレにならず、逆にグリッドが消えることもない＝実質無限グリッド。
    const float kTargetPx = 16.0f;
    float level = clamp(log10(kTargetPx * max(deriv.x, deriv.y)), 0.0f, 8.0f);  // 0=1m, 1=10m...
    float lvl0  = floor(level);
    float frc   = level - lvl0;

    // 線は「明るい芯」＋「その外側の暗い縁取り」の 2 層で描く。空のような明るい背景でも
    // 夜/暗い床でも必ずどちらかがコントラストを持つ＝背景色に依存せず読める。
    const float kMinorPx    = 1.1f;
    const float kMajorPx    = 1.8f;
    const float kAxisPx     = 2.4f;
    const float kHaloPx     = 1.5f;    // 縁取りは芯より何 px 太いか
    const float kMinorAlpha = 0.22f;   // 細線の濃さ（旧 0.45。主張しすぎるので半減）
    const float kMajorAlpha = 0.42f;   // 10 本ごとの太線（旧 0.80）
    const float kAxisAlpha  = 0.62f;   // X/Z 軸（旧 1.00）
    const float kHaloAlpha  = 0.60f;   // 縁取りは芯の何割か

    const float3 kMinorColor = float3(0.86f, 0.87f, 0.90f);
    const float3 kMajorColor = float3(1.00f, 1.00f, 1.00f);
    const float3 kAxisXColor = float3(1.00f, 0.34f, 0.36f);
    const float3 kAxisZColor = float3(0.36f, 0.62f, 1.00f);
    const float3 kHaloColor  = float3(0.02f, 0.02f, 0.03f);

    // ★桁を 3 段ぶん描くのは「桁上がりでの点滅」を消すため。
    //   線群 10^k の重みを d = k - level の連続関数にすると、level が上がるにつれ
    //   同じ線群が 太線 → 細線 → 消滅 と滑らかに移り変わる。2 段だと一番粗い段が
    //   「細線扱い ↔ 太線扱い」を境界でジャンプするので、そこだけ必ずパカパカする。
    float3 coreColor = kMinorColor;
    float  coreAlpha = 0.0f;
    float  haloAlpha = 0.0f;
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        float  cell = pow(10.0f, lvl0 + (float)i);
        float  d    = (float)i - frc;               // 最小セルより何桁粗いか
        float  maj  = saturate(d);                  // 0=細線 1=太線
        float  wgt  = lerp(kMinorAlpha, kMajorAlpha, maj) * saturate(d + 1.0f);
        // 位相は「カメラ位置の小数部 + 相対位置」で作る。原点から遠く離れても
        // local 側の精度が落ちない（cameraPos.xz を直接足すと桁落ちする）。
        float2 uv = frac(cameraPos.xz / cell) + local / cell;
        float2 ud = max(deriv / cell, 1e-8f);
        float  c  = GridCoverage(uv, ud, lerp(kMinorPx, kMajorPx, maj));
        float  h  = GridCoverage(uv, ud, lerp(kMinorPx, kMajorPx, maj) + kHaloPx);

        float a   = c * wgt;
        coreColor = (a > coreAlpha) ? lerp(kMinorColor, kMajorColor, maj) : coreColor;
        coreAlpha = max(coreAlpha, a);
        haloAlpha = max(haloAlpha, h * wgt * kHaloAlpha);
    }

    // 軸線。ライティング/シャドウは掛けない＝ライトの当たり方や影で線が沈まない。
    float  worldX = cameraPos.x + local.x;
    float  worldZ = cameraPos.z + local.y;
    float  axisX  = AxisCoverage(worldZ, deriv.y, kAxisPx);   // +X 軸 (z=0)
    float  axisZ  = AxisCoverage(worldX, deriv.x, kAxisPx);   // +Z 軸 (x=0)
    float  axisH  = max(AxisCoverage(worldZ, deriv.y, kAxisPx + kHaloPx),
                        AxisCoverage(worldX, deriv.x, kAxisPx + kHaloPx));
    coreColor = lerp(coreColor, kAxisXColor, axisX);
    coreColor = lerp(coreColor, kAxisZColor, axisZ);
    coreAlpha = max(coreAlpha, max(axisX, axisZ) * kAxisAlpha);
    haloAlpha = max(haloAlpha, axisH * kAxisAlpha * kHaloAlpha);

    // 芯を縁取りの上に合成して 1 枚のストレートアルファに畳む。
    // グリッド色は表示基準(sRGB風)で調整してあるので、合成はリニアへ直してから行う
    // （シーン RT はリニア HDR。最終段で ACES+ガンマが掛かる）。
    float  outAlpha = coreAlpha + haloAlpha * (1.0f - coreAlpha);
    float3 outColor = (pow(coreColor, 2.2f) * coreAlpha
                     + pow(kHaloColor, 2.2f) * haloAlpha * (1.0f - coreAlpha))
                    / max(outAlpha, 1e-4f);

    // 距離フェード: 原点からではなく「カメラの真下」から測る＝カメラと一緒にグリッドが
    // ついてくる。板は ±10km(kEditorGridSize/2)あるので、どこへ飛んでも足元に線がある。
    // フェード半径はカメラ高度に比例させる: 地面に近いほど手元だけ、上へ引くほど広く出す。
    // ★下限は 400m ではなく 20m。400m だとカメラ高 1.7m の一人称でも fadeStart=220m に
    //   なり、30m 程度の屋内シーンではフェードが一切効かず、床全面にグリッドの膜が
    //   乗ったままになる（グリッド板は y=0 で床の上面と同一平面、かつ DepthBias が
    //   負なので必ず床に勝つ）。20m なら fadeEnd≈24m / fadeStart≈13m で足元だけに出る。
    float fadeEnd   = clamp(abs(cameraPos.y - model._42) * 14.0f, 20.0f, 9000.0f);
    float fadeStart = fadeEnd * 0.55f;
    float dist      = length(local);
    outAlpha *= 1.0f - saturate((dist - fadeStart) / max(fadeEnd - fadeStart, 1e-3f));

    return float4(outColor, outAlpha);
}
