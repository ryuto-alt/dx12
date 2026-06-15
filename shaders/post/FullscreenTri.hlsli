// 頂点バッファ不要のフルスクリーン三角形 VS。
// SV_VertexID 0,1,2 から画面全体を覆う 1 枚の三角形を生成する。
// 各ポストエフェクト HLSL がこれを #include して VS を共有する。

struct FSQuadVSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

FSQuadVSOut FSTriVS(uint id : SV_VertexID)
{
    FSQuadVSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);          // (0,0) (2,0) (0,2)
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
