// ===========================================================================
// BrdfV2.hlsli — UNO_SHADING_V2 の枠（W1-C3 MH-0a。中身は空）。
//
//   -D UNO_SHADING_V2=1 でコンパイルした .cso（shaders/manifest/lane_c.cmake の *V2_PS）と、
//   ForwardGraph の実行時コンパイル（shadingRevision==2 のとき）だけが、この枠の内側を見る。
//   V1 の .cso は UNO_SHADING_V2 を定義しないので、このファイルを #include しても 1 命令も変わらない。
//
//   ★今は誰もこのファイルを #include していない。#include を足す（C4 / C5 / MH-2）ときは、同じ変更で
//     src/resource/ShaderRegistry.cpp の依存表（staticDeps）にも "forward/BrdfV2.hlsli" を足すこと
//     （ShaderIncludeDeps テストが漏れを検出する）。
//
//   入る予定のもの（docs/SHADING_V2.md の表）:
//     C5   マイクロシャドウ / GTSO / horizon occlusion（PfSlot::ShadingV2 の shadingFlags で個別に切る）
//     MH-2 直接光の BRDF v2（height-correlated Smith、EON の拡散、層の合成、球と太陽の円盤、F82）
// ===========================================================================
#ifndef UNO_BRDF_V2_HLSLI
#define UNO_BRDF_V2_HLSLI

#ifdef UNO_SHADING_V2

// PfSlot::ShadingV2 の shadingFlags（src/renderer/FrameConstants.h と 1 対 1）。
// 値は ShadingSettings の microShadow / horizonOcclusion / gtso に対応する（C5 が確定させる。ここでは予約だけ）。
// static const uint UNO_V2_FLAG_MICRO_SHADOW = 1u << 0;
// static const uint UNO_V2_FLAG_HORIZON_OCC  = 1u << 1;
// static const uint UNO_V2_FLAG_GTSO         = 1u << 2;

#endif // UNO_SHADING_V2

#endif // UNO_BRDF_V2_HLSLI
