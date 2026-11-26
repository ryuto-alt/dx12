# レーン B のシェーダ登録（ルート CMakeLists.txt の DXC 区画から include される。W05-16 で配線）。
# 自分のブロックの中にだけ足す（ブロックの外は書き換えない）。
# ==== lane-B:shaders begin ====
# UnoFX（W1-BD4）。SM 6.6（ResourceDescriptorHeap[]）。NEW = -HV 2021 -WX
uno_shader(NAME VfxEmit_CS      FILE vfx/VfxSim.hlsl  ENTRY CSEmit      PROFILE cs_6_6 NEW)
uno_shader(NAME VfxSim_CS       FILE vfx/VfxSim.hlsl  ENTRY CSSim       PROFILE cs_6_6 NEW)
uno_shader(NAME VfxClear_CS     FILE vfx/VfxSim.hlsl  ENTRY CSClear     PROFILE cs_6_6 NEW)
uno_shader(NAME VfxStatClear_CS FILE vfx/VfxSim.hlsl  ENTRY CSStatClear PROFILE cs_6_6 NEW)
uno_shader(NAME VfxDraw_VS      FILE vfx/VfxDraw.hlsl ENTRY VSMain      PROFILE vs_6_6 NEW)
uno_shader(NAME VfxDraw_PS      FILE vfx/VfxDraw.hlsl ENTRY PSMain      PROFILE ps_6_6 NEW)
uno_shader(NAME VfxCurlProbe_CS FILE vfx/VfxCurlProbe.hlsl ENTRY CSProbe PROFILE cs_6_6 NEW)   # 試験用（tests/vfx_gpu_test.cpp）
# ==== lane-B:shaders end ====
