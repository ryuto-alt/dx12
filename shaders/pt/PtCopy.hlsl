// PtCopy.hlsl — バッファのコピー(compute)。パストレーサーがスキンド頂点の変形後位置を
// 「ジョブ専用のバッファ」へ写し取るために使う(SkinningCompute の出力はフレームごとに書き換わるため)。
// ルートシグネチャ: b0 = 32bit 定数(コピーする DWORD 数) / t0 = 元(ルート SRV・アドレス直渡し) / u0 = 先(ルート UAV)
cbuffer CopyCB : register(b0) { uint gWords; uint gPad0; uint gPad1; uint gPad2; };
ByteAddressBuffer   gSrc : register(t0);
RWByteAddressBuffer gDst : register(u0);

[numthreads(256, 1, 1)]
void CopyCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWords) return;
    gDst.Store(id.x * 4, gSrc.Load(id.x * 4));
}
