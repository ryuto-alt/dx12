#pragma once
// ===========================================================================
// 読み取り専用 DSV の記述子（FU-E-21）。AfterTranslucent の StageCtx::depthDsvReadOnly に使う。
//   深度のテストだけ行い、書き込まない。同じ深度を SRV としても読める（DEPTH_READ | PIXEL_SHADER_RESOURCE）ので、
//   ソフトパーティクル等がハードウェアの深度テストと深度の読み出しを同時に行える。
//   深度リソースは R32_TYPELESS（DSV は D32_FLOAT）で作られている前提。それ以外は 0 を返す側（呼び出し側）で見送る。
// ===========================================================================
#include <directx/d3d12.h>

namespace dx12e
{

// 深度リソースの記述から、読み取り専用 DSV を作れるか。
inline bool CanMakeReadOnlyDsv(const D3D12_RESOURCE_DESC& d)
{
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
        && d.SampleDesc.Count == 1
        && d.DepthOrArraySize == 1
        && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0
        && (d.Format == DXGI_FORMAT_R32_TYPELESS || d.Format == DXGI_FORMAT_D32_FLOAT);
}

inline D3D12_DEPTH_STENCIL_VIEW_DESC MakeReadOnlyDsvDesc()
{
    D3D12_DEPTH_STENCIL_VIEW_DESC v{};
    v.Format        = DXGI_FORMAT_D32_FLOAT;
    v.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    v.Flags         = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    return v;
}

} // namespace dx12e
