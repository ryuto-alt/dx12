#pragma once
//
// Gribb-Hartmann の視錐台抽出（D3D 用: 行ベクトル規約・クリップ z∈[0,1]）。
// world*viewProj に使うのと同じ行優先 viewProj から 6 平面を作り、ワールド空間の
// バウンディング球で「画面に少しでも入るか」を判定する。CPU カリング専用・GPU 非依存。
//
// 使い方:
//   Frustum f = Frustum::FromViewProj(viewProj);
//   if (!f.SphereVisible(center, radius)) { /* 画面外: 描画スキップ */ }
//
#include <DirectXMath.h>

namespace dx12e
{

struct Frustum
{
    // 各平面 = (a,b,c,d)（正規化済み）。点が内側なら a*x+b*y+c*z+d >= 0。
    DirectX::XMVECTOR planes[6];

    static Frustum FromViewProj(DirectX::FXMMATRIX viewProj)
    {
        using namespace DirectX;
        // 転置すると行 r0..r3 が viewProj の「列」になる。p(行ベクトル)·viewProj の各成分は
        // p と列の内積なので、列の線形結合がそのままクリップ平面になる。
        XMMATRIX m = XMMatrixTranspose(viewProj);
        const XMVECTOR r0 = m.r[0], r1 = m.r[1], r2 = m.r[2], r3 = m.r[3];
        Frustum f;
        f.planes[0] = XMVectorAdd(r3, r0);       // left   (x' > -w')
        f.planes[1] = XMVectorSubtract(r3, r0);  // right  (x' <  w')
        f.planes[2] = XMVectorAdd(r3, r1);       // bottom (y' > -w')
        f.planes[3] = XMVectorSubtract(r3, r1);  // top    (y' <  w')
        f.planes[4] = r2;                        // near   (z' >  0) ← D3D は [0,1]
        f.planes[5] = XMVectorSubtract(r3, r2);  // far    (z' <  w')
        for (int i = 0; i < 6; ++i)
            f.planes[i] = XMPlaneNormalize(f.planes[i]);  // xyz 長で正規化
        return f;
    }

    // ワールド空間の球が視錐台と少しでも交差すれば true（保守的: 偽陰性を出さない）。
    bool SphereVisible(DirectX::FXMVECTOR center, float radius) const
    {
        using namespace DirectX;
        for (int i = 0; i < 6; ++i)
            if (XMVectorGetX(XMPlaneDotCoord(planes[i], center)) < -radius)
                return false;   // この平面の完全に外側 → カリング
        return true;
    }
};

} // namespace dx12e
