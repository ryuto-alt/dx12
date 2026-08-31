// VelocityCommon.hlsli - 深度プリパス（PrepassMode::DepthVelocityGBuffer）の共通定義。
// このパスは MRT=2 で「速度バッファ」と「G-Buffer」を同時に書く。
//
// ★この2点を絶対に混同しないこと:
//   - SV_POSITION は「ジッタ込み」。フォワード/深度プリパスとクリップZをビット一致させるため必須。
//     ずらすと forward の LESS_EQUAL で面が欠落する。
//   - 速度に書く値は「ジッタを完全に除去した」現・前フレームの NDC 差分（＝本当の動き）。
//     静止時に速度が 0 でないと TAA の履歴再投影が毎フレーム破綻する。
//
//   velocity = (curNDC - jitter) - (prevNDC - prevJitter)
//   前フレームのクリップ座標は最初から「ジッタなし viewProj」で作るので prevJitter は 0。
//
//   出典: https://alextardif.com/TAA.html
//         https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/
//
// ★G-Buffer（計画04 の SSR/SSGI 用）:
//   法線は forward と完全に同じ式 mul(normal, (float3x3)model) で作った**ワールド空間**法線。
//   ビュー空間へ回すのは読む側（SSR/SSGI）の仕事。
//   roughness/metallic は b0 に 1 float へ詰めて渡す（b0 は 40 DWORD しか無い）。
//   v1 では法線マップ / metalRoughness テクスチャは読まない（プリパスで t0-t2 の
//   ディスクリプタテーブルを毎ドロー貼るとプリパスの安さが失われるため）。
#ifndef VELOCITY_COMMON_HLSLI
#define VELOCITY_COMMON_HLSLI

#include "../screenspace/ScreenSpaceCommon.hlsli"

struct VelocityVSOut
{
    float4 posSV       : SV_POSITION;  // ジッタ込みクリップ座標（深度をフォワードと一致させる）
    float4 curClip     : TEXCOORD0;    // ジッタ除去済み 現フレームクリップ座標
    float4 prevClip    : TEXCOORD1;    // 前フレーム(非ジッタ)クリップ座標
    float3 worldNormal : NORMAL;       // ワールド空間法線（正規化前）
    float2 material    : TEXCOORD2;    // x=roughness y=metallic（VS で b0 から展開済み）
#ifdef ALPHA_TEST
    float2 uv          : TEXCOORD3;    // アルファクリップ(MASK)バリアントだけが持つ
#endif
};

#ifdef ALPHA_TEST
// ---- アルファクリップ(MASK)バリアント（-D ALPHA_TEST=1）--------------------
// ★これが無いと TAA(=速度プリパス)を入れた瞬間に「葉の隙間に板の深度が書かれ、
//   その裏の物が forward の LESS_EQUAL で弾かれて真っ黒に抜ける」。
//   Forward.hlsl / ShadowMask.hlsl と **同じテクスチャ・同じ cutoff・同じ UV** を読むこと。
//   1 つでもズレると本体と深度が食い違い、輪郭に黒い縁が出る。
// ★MASK のドローでだけこの PSO に切り替える（t0-t2 と b2 を貼る手間もそのぶんだけ）。
Texture2D    g_albedo  : register(t0);
SamplerState g_sampler : register(s0);

cbuffer PBRMaterial : register(b2)
{
    float  vpbrMetallic;
    float  vpbrRoughness;
    uint   vpbrFlags;          // bit8..15 = alphaCutoff(8bit 量子化)。renderer/Material.h 参照
    uint   vpbrPackedTint;
    float4 vpbrUvScaleOffset;  // xy=スケール zw=オフセット（無効時は (1,1,0,0)）
};
#endif

struct PrepassOut
{
    float2 velocity : SV_TARGET0;   // R16G16_FLOAT  現UV - 前UV
    float4 gbuffer  : SV_TARGET1;   // R16G16B16A16_FLOAT  xy=oct(worldN) z=rough w=metal
};

// 速度の符号規約: velocity = 現フレームUV - 前フレームUV
// 解決側は historyUV = uv - velocity で読む。ここを逆にすると全部逆向きにゴーストする。
// 単位は「ビューポートローカル UV」ではなく「NDC 差分 × (0.5,-0.5)」＝ビューポート全体を
// 1.0 とする UV。ビューポート矩形は clip 空間には現れないので、これがそのままローカル UV になる。
PrepassOut VelocityPS(VelocityVSOut i, bool isFront : SV_IsFrontFace)
{
    PrepassOut o;

#ifdef ALPHA_TEST
    {
        const float2 uv     = i.uv * vpbrUvScaleOffset.xy + vpbrUvScaleOffset.zw;
        const float  cutoff = float((vpbrFlags >> 8) & 0xFF) / 255.0;
        clip(g_albedo.Sample(g_sampler, uv).a - cutoff);
    }
#endif

    float2 curNdc  = i.curClip.xy  / max(abs(i.curClip.w),  1e-6);
    float2 prevNdc = i.prevClip.xy / max(abs(i.prevClip.w), 1e-6);
    // NDC(-1..1, y上) -> UV(0..1, y下): uv = ndc * (0.5, -0.5) + 0.5
    o.velocity = (curNdc - prevNdc) * float2(0.5, -0.5);

    // 背面ポリゴン（両面マテリアル / 内向きの箱＝部屋）は法線を反転して視線側へ向ける。
    // プリパスにはビュー空間位置が無いので SV_IsFrontFace で判定する（計画04 §6.4 の未確認 #3）。
    // 深度プリパスの PSO は CULL_NONE なので、この分岐は実際に使われる。
    float3 n = normalize(i.worldNormal);
    if (!isFront) n = -n;

    o.gbuffer = SS_PackGBuffer(n, i.material.x, i.material.y);
    return o;
}

#endif // VELOCITY_COMMON_HLSLI
