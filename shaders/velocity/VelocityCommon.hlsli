// VelocityCommon.hlsli - 速度バッファ(モーションベクター)生成パスの共通定義。
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
#ifndef VELOCITY_COMMON_HLSLI
#define VELOCITY_COMMON_HLSLI

struct VelocityVSOut
{
    float4 posSV    : SV_POSITION;  // ジッタ込みクリップ座標（深度をフォワードと一致させる）
    float4 curClip  : TEXCOORD0;    // ジッタ除去済み 現フレームクリップ座標
    float4 prevClip : TEXCOORD1;    // 前フレーム(非ジッタ)クリップ座標
};

// 速度の符号規約: velocity = 現フレームUV - 前フレームUV
// 解決側は historyUV = uv - velocity で読む。ここを逆にすると全部逆向きにゴーストする。
// 単位は「ビューポートローカル UV」ではなく「NDC 差分 × (0.5,-0.5)」＝ビューポート全体を
// 1.0 とする UV。ビューポート矩形は clip 空間には現れないので、これがそのままローカル UV になる。
float2 VelocityPS(VelocityVSOut i) : SV_TARGET
{
    float2 curNdc  = i.curClip.xy  / max(abs(i.curClip.w),  1e-6);
    float2 prevNdc = i.prevClip.xy / max(abs(i.prevClip.w), 1e-6);
    // NDC(-1..1, y上) -> UV(0..1, y下): uv = ndc * (0.5, -0.5) + 0.5
    return (curNdc - prevNdc) * float2(0.5, -0.5);
}

#endif // VELOCITY_COMMON_HLSLI
