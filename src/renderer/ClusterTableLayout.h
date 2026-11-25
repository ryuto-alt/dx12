#pragma once
// slot11（RootSignature::kSlotClusterSRV）のディスクリプタテーブルの宣言表。唯一の正本（W05-24）。
//
// ★ヒープ上の並び = kClusterRanges の宣言順（D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND）。
//   RootSignature.cpp のレンジ、ClusteredLightCulling のブロック確保、GraphPreviewRenderer の
//   ダミーのブロック確保は、すべてこの表から作る。増やすのはここだけ（ルート定数の DWORD は増えない）。
// ★将来分（LS-06 / MH-5 など）のレンジも W0.5 で全部宣言しておく。後からレンジを足すとルートシグネチャが
//   変わって全 PSO に波及するため。宣言だけで、シェーダがまだ読まない枠は DummyKind のダミー SRV で埋める。
// ★先頭 14 本（kClusterTableExisting）は既存。実体は各システム（ClusteredLightCulling / DecalSystem /
//   DDGI / LightCookieSystem / ReflectionProbeSystem）が上書きするので、ここのダミーの型は「その枠の本来の型」を表すだけ。
//   15 本目以降だけが、この表のダミー（ClusteredLightCulling::WriteDummySrv）で埋められる。

#include "core/Types.h"

namespace dx12e
{

enum class DummyKind : u8 { Buffer, Tex2D, Tex2DArray, Tex3D, TexCube, TexCubeArray };

struct ClusterRange
{
    u32         baseRegister;   // t<baseRegister> から count 本
    u32         count;
    DummyKind   dummy;
    const char* owner;
};

inline constexpr ClusterRange kClusterRanges[] = {
    // ---- 既存（14 本）。型は shaders/forward の宣言に合わせる ----
    {13, 3, DummyKind::Buffer,       "既存 クラスタ（ライト / インデックス / カウント）"},
    {18, 3, DummyKind::Buffer,       "既存 デカール t18..t20"},
    {21, 1, DummyKind::Tex2D,        "既存 デカールのアトラス t21"},
    {22, 2, DummyKind::Tex2D,        "既存 DDGI"},
    {30, 1, DummyKind::Tex2D,        "既存 DDGI プローブデータ"},
    {31, 1, DummyKind::Tex2DArray,   "既存 スポットのクッキー"},
    {32, 1, DummyKind::Tex2D,        "既存 IES"},
    {33, 1, DummyKind::TexCubeArray, "既存 反射プローブのキューブ配列"},
    {34, 1, DummyKind::Buffer,       "既存 反射プローブ情報"},
    // ---- 将来分（W0.5 で宣言。ダミーで埋める）----
    {35, 1, DummyKind::Tex2DArray,   "LS-06 LTC"},
    {46, 1, DummyKind::Buffer,       "MH-5 SH9"},
    {36, 1, DummyKind::Buffer,       "LS-08 ShadowTileGpu"},
    {27, 1, DummyKind::Buffer,       "MH-9 MaterialExtra"},
    {37, 2, DummyKind::Tex2D,        "LS-09 RT ローカル影"},
    {39, 1, DummyKind::Tex2D,        "MH-6 波紋"},
    {42, 1, DummyKind::Tex2D,        "MH-7 シーン色"},
    {41, 1, DummyKind::Tex2D,        "MH-10 肌 LUT"},
    {43, 1, DummyKind::Tex2D,        "MH-12 薄膜"},
    {40, 1, DummyKind::Tex2D,        "LS-01 GTSO LUT（予約）"},
    {28, 1, DummyKind::Tex2D,        "雲の影（予約）"},
    {49, 2, DummyKind::Tex2D,        "予約"},
};
inline constexpr u32 kClusterRangeCount = static_cast<u32>(sizeof(kClusterRanges) / sizeof(kClusterRanges[0]));

namespace cluster_table_detail
{
constexpr u32 SumCount(u32 n)
{
    u32 s = 0;
    for (u32 i = 0; i < n; ++i) s += kClusterRanges[i].count;
    return s;
}
}  // namespace cluster_table_detail

// ヒープ上のテーブル長（count の総和）。
inline constexpr u32 kClusterTableCount = cluster_table_detail::SumCount(kClusterRangeCount);
// 先頭の既存分（0..13）の本数。ここまでは各システムが実体を書く。
inline constexpr u32 kClusterTableExisting = 14;

// baseRegister のテーブル内オフセット（ヒープ上の位置）。表に無ければ 0xFFFFFFFF。
constexpr u32 ClusterTableOffsetOf(u32 baseRegister)
{
    u32 off = 0;
    for (u32 i = 0; i < kClusterRangeCount; ++i)
    {
        if (kClusterRanges[i].baseRegister == baseRegister) return off;
        off += kClusterRanges[i].count;
    }
    return 0xFFFFFFFFu;
}

// テーブル内オフセット index（0..kClusterTableCount-1）の枠の DummyKind。
constexpr DummyKind ClusterDummyKindAt(u32 index)
{
    u32 off = 0;
    for (u32 i = 0; i < kClusterRangeCount; ++i)
    {
        if (index < off + kClusterRanges[i].count) return kClusterRanges[i].dummy;
        off += kClusterRanges[i].count;
    }
    return DummyKind::Buffer;
}

}  // namespace dx12e
