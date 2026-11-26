#pragma once
// 材質テーブル（RootSignature::kSlotPBRTextures = slot 2）の SRV 枚数。W05-24。
// t0 albedo / t1 normal / t2 metalRoughness / t24 emissive / t25 height の 5 枚（RootSignature.cpp の pbrRanges）。
// MH-9 が t44 / t45（ディテール）を足して 7 にする。そのとき Material.h の kMaterialSrvBlockSize と一緒に変える。

#include "core/Types.h"

namespace dx12e
{

inline constexpr u32 kMaterialTableCount = 5;

}  // namespace dx12e
