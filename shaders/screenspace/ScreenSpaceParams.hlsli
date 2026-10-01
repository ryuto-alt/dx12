// ScreenSpaceParams.hlsli - SSR / SSGI / カラー縮小が共有する cbuffer とリソース宣言。
// C++ 側は src/renderer/ScreenSpaceGiPass.cpp の SsParamsCB とバイト単位で一致させること。
#ifndef SCREENSPACE_PARAMS_HLSLI
#define SCREENSPACE_PARAMS_HLSLI

#include "ScreenSpaceCommon.hlsli"

// 6 本のテーブルは全パスで必ずバインドされる（未参照のリソースは DXC が除去するので、
// そのパスが使わないレジスタを宣言していても PSO は壊れない）。
Texture2D<float>    g_depth     : register(t0);   // カメラ深度 R32_FLOAT（フル解像度）
Texture2D<float4>   g_gbuffer   : register(t1);   // xy=oct(worldN) z=rough w=metal（フル解像度）
Texture2D<float4>   g_colorA    : register(t2);   // 前フレームカラー(フル) / トレース結果 / 履歴
Texture2D<float4>   g_colorB    : register(t3);   // 前フレームカラー(ハーフ) / 履歴
Texture2D<float2>   g_velocity  : register(t4);   // R16G16_FLOAT（ビューポートローカル UV 単位）
TextureCube<float4> g_irradiance: register(t5);   // SSGI のミス時フォールバック

// GI モード New の SSGI ミス → DDGI 用（RS の 7 本目のテーブル = t6..t8。フォワードのクラスタテーブルの
// DDGI 3 本 [t22 irradiance / t23 距離 / t30 プローブデータ] をそのまま指す）。
// Legacy / DDGI が無いときは gDdgi0.w = 0 で一切読まない（黒ダミーが貼られている）。
Texture2D<float4>   g_ddgiIrradiance : register(t6);
Texture2D<float2>   g_ddgiDistance   : register(t7);
Texture2D<float4>   g_ddgiProbeData  : register(t8);
#define DDGI_PROBEDATA_LOAD(t) g_ddgiProbeData.Load(int3(int2(t), 0))
#include "../ddgi/DdgiCommon.hlsli"

SamplerState g_pointClamp  : register(s0);
SamplerState g_linearClamp : register(s1);

cbuffer SsParams : register(b0)
{
    float4x4 gProj;        // 転置済み（ビュー→クリップ）
    float4x4 gInvProj;     // 転置済み（クリップ→ビュー）
    float4x4 gView;        // 転置済み（ワールド→ビュー）
    float4x4 gInvView;     // 転置済み（ビュー→ワールド。SSGI が方向をワールドへ戻す用）

    float4 gRT;            // xy = 1/フルRTサイズ, zw = 1/ハーフRTサイズ
    float4 gViewport;      // xy = 原点(px), zw = サイズ(px)  ※フル解像度基準

    float4 gSsr0;          // x=maxDistance y=thickness z=stride w=bias
    float4 gSsr1;          // x=maxSteps y=roughnessCutoff z=edgeFade w=intensity

    float4 gSsgi0;         // x=radius y=thickness z=rayCount w=stepCount
    float4 gSsgi1;         // x=intensity y=clampValue z=feedback w=iblFallback

    float4 gMisc;          // x=zNear y=zFar z=historyValid w=hasIBL
    float4 gMisc2;         // x=フレーム連番(時間ジッタ用) y=DDGI の内部単位 → シーン単位の倍率(GI モード New) zw=予約

    float4 gDdgi0;         // xyz=DDGI 格子の原点 w=GI モード New で DDGI が使える(1/0)
    float4 gDdgi1;         // xyz=プローブ間隔 w=視線バイアス(m)
    float4 gDdgi2;         // xyz=プローブ数（1 カスケードぶん）w=法線バイアス(m)
    float4 gDdgi3;         // xyz=カスケード 1 の窓の原点 w=カスケード 1 のプローブ間隔（GI S4）
    float4 gDdgi4;         // xyz=カスケード 0 の記憶領域のずらし w=カスケード数（0/1 = 固定ボリューム）
    float4 gDdgi5;         // xyz=カスケード 1 の記憶領域のずらし w=予約
};

#define SS_INV_RT   (gRT.xy)
#define SS_INV_HALF (gRT.zw)

// 共通: フル RT UV から表面情報を引く。深度が背景なら false。
bool SS_LoadSurface(float2 uv, out float3 viewPos, out SSSurface surf, out float3 worldN)
{
    float d = g_depth.SampleLevel(g_pointClamp, uv, 0);
    viewPos = SS_ViewPosFromDepth(uv, d, gInvProj, SS_INV_RT, gViewport);
    surf    = SS_UnpackGBuffer(g_gbuffer.SampleLevel(g_pointClamp, uv, 0));
    worldN  = surf.N;
    return d < 1.0;
}

// ワールド法線 → ビュー空間法線
float3 SS_ToViewNormal(float3 worldN)
{
    return normalize(mul(float4(worldN, 0.0), gView).xyz);
}

#endif // SCREENSPACE_PARAMS_HLSLI
