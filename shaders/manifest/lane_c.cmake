# レーン C のシェーダ登録（ルート CMakeLists.txt の DXC 区画から include される。W05-16 で配線）。
# 自分のブロックの中にだけ足す（ブロックの外は書き換えない）。
# ==== lane-C:shaders begin ====
# --- MH-0a / W1-C3: UNO_SHADING_V2 の軸 ---------------------------------------------------------------------
# V1 の .cso（上の ShaderManifest.cmake）は 1 文字も変えず、V2 は「別の .cso」として並べる（NEW = -HV 2021 -WX）。
# プロファイルは V1 と同じ（SM 6.9 への引き上げは F 段で別の .cso として行う）。
# 名前の規則: <V1 の名前から _PS を除いたもの>V2_PS。選ぶのは ApplicationPipeline.cpp（shadingRevision==2 のとき）。
# 中身はまだ V1 と同じ命令列（BrdfV2.hlsli は空の枠。マイクロシャドウ・GTSO・地平線は C5 以降で入る）。
# Forward（不透明 / MASK / 物理ライティング単位。スキンドも Forward_PS を共用）
uno_shader(NAME ForwardV2_PS          FILE forward/Forward.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES UNO_SHADING_V2=1 NEW)
uno_shader(NAME ForwardPhysV2_PS      FILE forward/Forward.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES UNO_PHYSICAL_LIGHTS=1 UNO_SHADING_V2=1 NEW)
uno_shader(NAME ForwardMaskV2_PS      FILE forward/Forward.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES ALPHA_TEST=1 UNO_SHADING_V2=1 NEW)
uno_shader(NAME ForwardMaskPhysV2_PS  FILE forward/Forward.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES ALPHA_TEST=1 UNO_PHYSICAL_LIGHTS=1 UNO_SHADING_V2=1 NEW)
# 地形
uno_shader(NAME TerrainV2_PS          FILE forward/Terrain.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES UNO_SHADING_V2=1 NEW)
uno_shader(NAME TerrainPhysV2_PS      FILE forward/Terrain.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES UNO_PHYSICAL_LIGHTS=1 UNO_SHADING_V2=1 NEW)
# 仮想ジオメトリの解決
uno_shader(NAME VgResolveV2_PS        FILE vg/VgResolve.hlsl ENTRY PSMain PROFILE ps_6_6 DEFINES UNO_SHADING_V2=1 NEW)
# 植生
uno_shader(NAME ForwardFoliageV2_PS         FILE foliage/ForwardFoliage_PS.hlsl ENTRY FoliagePS PROFILE ps_6_0 DEFINES UNO_SHADING_V2=1 NEW)
uno_shader(NAME ForwardFoliagePhysV2_PS     FILE foliage/ForwardFoliage_PS.hlsl ENTRY FoliagePS PROFILE ps_6_0 DEFINES UNO_PHYSICAL_LIGHTS=1 UNO_SHADING_V2=1 NEW)
uno_shader(NAME ForwardFoliageMaskV2_PS     FILE foliage/ForwardFoliage_PS.hlsl ENTRY FoliagePS PROFILE ps_6_0 DEFINES FOLIAGE_MASK=1 UNO_SHADING_V2=1 NEW)
uno_shader(NAME ForwardFoliageMaskPhysV2_PS FILE foliage/ForwardFoliage_PS.hlsl ENTRY FoliagePS PROFILE ps_6_0 DEFINES FOLIAGE_MASK=1 UNO_PHYSICAL_LIGHTS=1 UNO_SHADING_V2=1 NEW)
# 水（水面と水中）
uno_shader(NAME WaterSurfaceV2_PS     FILE water/WaterSurface.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES UNO_SHADING_V2=1 NEW)
uno_shader(NAME WaterSurfacePhysV2_PS FILE water/WaterSurface.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES UNO_PHYSICAL_LIGHTS=1 UNO_SHADING_V2=1 NEW)
uno_shader(NAME WaterUnderV2_PS       FILE water/WaterUnder.hlsl ENTRY PSUnder PROFILE ps_6_0 DEFINES UNO_SHADING_V2=1 NEW)
uno_shader(NAME WaterUnderPhysV2_PS   FILE water/WaterUnder.hlsl ENTRY PSUnder PROFILE ps_6_0 DEFINES UNO_PHYSICAL_LIGHTS=1 UNO_SHADING_V2=1 NEW)
# ==== lane-C:shaders end ====
