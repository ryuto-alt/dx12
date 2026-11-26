# シェーダの宣言表（MH-0b / W05-16）。.cso は 1 本ずつ uno_shader() で宣言する。
# ・既存の .cso の DXC コマンドラインは、ここへ移す前後で 1 文字も変えない（NEW を付けない）。
# ・新しく足す .cso は、各レーンが shaders/manifest/lane_{a,b,c,d}.cmake の自分のブロックに書く（ここは触らない）。
# ・NEW を付けた .cso にだけ -HV 2021 -WX が付く。PDB は PIX 用の .pdb を別に出す（任意）。
# ・include の依存は uno_shader() が configure 時に #include を辿って張る（cmake/UnoShaders.cmake）。
# 
# uno_shader(NAME <cso 名（.cso は付けない）> FILE <shaders/ からの相対パス> ENTRY <関数> PROFILE <ps_6_0 ...>
#            [DEFINES A=1 ...] [DEPENDS <追加の依存ファイル>...] [NEW] [PDB])


# --- atmosphere ---
# 物理ベース大気と、ボリュメトリッククラウド。
uno_shader(NAME AtmosphereIrradiance_CS FILE atmosphere/AtmosphereIrradiance.hlsl ENTRY CSMain PROFILE cs_6_0)
uno_shader(NAME AtmosphereAp_CS FILE atmosphere/AtmosphereLuts.hlsl ENTRY ApCS PROFILE cs_6_0)
uno_shader(NAME AtmosphereCubeMip_CS FILE atmosphere/AtmosphereLuts.hlsl ENTRY SkyCubeMipCS PROFILE cs_6_0)
uno_shader(NAME AtmosphereCube_CS FILE atmosphere/AtmosphereLuts.hlsl ENTRY SkyCubeCS PROFILE cs_6_0)
uno_shader(NAME AtmosphereMs_CS FILE atmosphere/AtmosphereLuts.hlsl ENTRY MultiScatteringCS PROFILE cs_6_0)
uno_shader(NAME AtmosphereSv_CS FILE atmosphere/AtmosphereLuts.hlsl ENTRY SkyViewCS PROFILE cs_6_0)
uno_shader(NAME AtmosphereT_CS FILE atmosphere/AtmosphereLuts.hlsl ENTRY TransmittanceCS PROFILE cs_6_0)
uno_shader(NAME AtmosphereAp_PS FILE atmosphere/AtmosphereSky.hlsl ENTRY PSAp PROFILE ps_6_0)
uno_shader(NAME AtmosphereSky_PS FILE atmosphere/AtmosphereSky.hlsl ENTRY PSSky PROFILE ps_6_0)
uno_shader(NAME AtmosphereSky_VS FILE atmosphere/AtmosphereSky.hlsl ENTRY VSFull PROFILE vs_6_0)
uno_shader(NAME CloudComposite_PS FILE atmosphere/CloudMarch.hlsl ENTRY PSComposite PROFILE ps_6_0)
uno_shader(NAME CloudComposite_VS FILE atmosphere/CloudMarch.hlsl ENTRY VSFull PROFILE vs_6_0)
uno_shader(NAME CloudCubeCloud_CS FILE atmosphere/CloudMarch.hlsl ENTRY CubeCloudCS PROFILE cs_6_0)
uno_shader(NAME CloudResolve_CS FILE atmosphere/CloudMarch.hlsl ENTRY ResolveCS PROFILE cs_6_0)
uno_shader(NAME CloudShadowApply_PS FILE atmosphere/CloudMarch.hlsl ENTRY PSShadowApply PROFILE ps_6_0)
uno_shader(NAME CloudShadowMap_CS FILE atmosphere/CloudMarch.hlsl ENTRY ShadowMapCS PROFILE cs_6_0)
uno_shader(NAME CloudTrace_CS FILE atmosphere/CloudMarch.hlsl ENTRY TraceCS PROFILE cs_6_0)
uno_shader(NAME CloudDetail_CS FILE atmosphere/CloudNoise.hlsl ENTRY DetailCS PROFILE cs_6_0)
uno_shader(NAME CloudShape_CS FILE atmosphere/CloudNoise.hlsl ENTRY ShapeCS PROFILE cs_6_0)
uno_shader(NAME CloudWeather_CS FILE atmosphere/CloudNoise.hlsl ENTRY WeatherCS PROFILE cs_6_0)

# --- ddgi ---
# DDGI（バインドレスのため SM 6.6）。
uno_shader(NAME DdgiBlendDist_CS FILE ddgi/DdgiProbeUpdate.hlsl ENTRY BlendDistanceCS PROFILE cs_6_6)
uno_shader(NAME DdgiBlend_CS FILE ddgi/DdgiProbeUpdate.hlsl ENTRY BlendCS PROFILE cs_6_6)
uno_shader(NAME DdgiGround_CS FILE ddgi/DdgiProbeUpdate.hlsl ENTRY GroundCS PROFILE cs_6_6)
uno_shader(NAME DdgiLightGrid_CS FILE ddgi/DdgiProbeUpdate.hlsl ENTRY LightGridCS PROFILE cs_6_6)
uno_shader(NAME DdgiProbeData_CS FILE ddgi/DdgiProbeUpdate.hlsl ENTRY ProbeDataCS PROFILE cs_6_6)
uno_shader(NAME DdgiTrace_CS FILE ddgi/DdgiProbeUpdate.hlsl ENTRY TraceCS PROFILE cs_6_6)

# --- debug ---
# デバッグ線。
uno_shader(NAME DebugLine_PS FILE debug/DebugLine.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME DebugLine_VS FILE debug/DebugLine.hlsl ENTRY VSMain PROFILE vs_6_0)

# --- editor ---
# エディタ専用（アイコンのビルボード、選択 / ホバーの輪郭）。ゲーム・Play では使わない。
uno_shader(NAME IconBillboard_PS FILE editor/IconBillboard.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME IconBillboard_VS FILE editor/IconBillboard.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME SelectionComposite_PS FILE editor/SelectionOutline.hlsl ENTRY PSComposite PROFILE ps_6_0)
uno_shader(NAME SelectionComposite_VS FILE editor/SelectionOutline.hlsl ENTRY VSFull PROFILE vs_6_0)
uno_shader(NAME SelectionFill_PS FILE editor/SelectionOutline.hlsl ENTRY PSFill PROFILE ps_6_0)
uno_shader(NAME SelectionMaskSkinned_VS FILE editor/SelectionOutline.hlsl ENTRY VSMaskSkinned PROFILE vs_6_0)
uno_shader(NAME SelectionMask_PS FILE editor/SelectionOutline.hlsl ENTRY PSMask PROFILE ps_6_0)
uno_shader(NAME SelectionMask_VS FILE editor/SelectionOutline.hlsl ENTRY VSMask PROFILE vs_6_0)
uno_shader(NAME SelectionWire_PS FILE editor/SelectionOutline.hlsl ENTRY PSWire PROFILE ps_6_0)

# --- fog ---
# ボリュメトリックフォグ（froxel）。
uno_shader(NAME FogComposite_PS FILE fog/FogComposite.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME FogComposite_VS FILE fog/FogComposite.hlsl ENTRY FSTriVS PROFILE vs_6_0)
uno_shader(NAME FogInject_CS FILE fog/FogInject.hlsl ENTRY CSInject PROFILE cs_6_0)
uno_shader(NAME FogIntegrate_CS FILE fog/FogIntegrate.hlsl ENTRY CSIntegrate PROFILE cs_6_0)
uno_shader(NAME FogScatter_CS FILE fog/FogScatter.hlsl ENTRY CSScatter PROFILE cs_6_0)

# --- foliage ---
# 植生（FoliageLayer + GPU カリング + ExecuteIndirect + 風）。
uno_shader(NAME FoliageCount_CS FILE foliage/FoliageCull.hlsl ENTRY CSCount PROFILE cs_6_0)
uno_shader(NAME FoliageEmit_CS FILE foliage/FoliageCull.hlsl ENTRY CSEmit PROFILE cs_6_0)
uno_shader(NAME FoliageFinalize_CS FILE foliage/FoliageCull.hlsl ENTRY CSFinalize PROFILE cs_6_0)
uno_shader(NAME FoliageReset_CS FILE foliage/FoliageCull.hlsl ENTRY CSReset PROFILE cs_6_0)
uno_shader(NAME FoliageScan_CS FILE foliage/FoliageCull.hlsl ENTRY CSScan PROFILE cs_6_0)
uno_shader(NAME FoliageDepthMask_PS FILE foliage/FoliageDepth.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES ALPHA_TEST=1)
uno_shader(NAME FoliageDepth_PS FILE foliage/FoliageDepth.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME FoliageDepth_VS FILE foliage/FoliageDepth.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME FoliageVelocityMask_PS FILE foliage/FoliageVelocity.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES ALPHA_TEST=1)
uno_shader(NAME FoliageVelocity_PS FILE foliage/FoliageVelocity.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME FoliageVelocity_VS FILE foliage/FoliageVelocity.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME FoliageWindTest_CS FILE foliage/FoliageWindTest.hlsl ENTRY CSWindTest PROFILE cs_6_0)
uno_shader(NAME ForwardFoliageMaskPhys_PS FILE foliage/ForwardFoliage_PS.hlsl ENTRY FoliagePS PROFILE ps_6_0 DEFINES FOLIAGE_MASK=1 UNO_PHYSICAL_LIGHTS=1)
uno_shader(NAME ForwardFoliageMask_PS FILE foliage/ForwardFoliage_PS.hlsl ENTRY FoliagePS PROFILE ps_6_0 DEFINES FOLIAGE_MASK=1)
uno_shader(NAME ForwardFoliagePhys_PS FILE foliage/ForwardFoliage_PS.hlsl ENTRY FoliagePS PROFILE ps_6_0 DEFINES UNO_PHYSICAL_LIGHTS=1)
uno_shader(NAME ForwardFoliage_PS FILE foliage/ForwardFoliage_PS.hlsl ENTRY FoliagePS PROFILE ps_6_0)
uno_shader(NAME ForwardFoliage_VS FILE foliage/ForwardFoliage_VS.hlsl ENTRY VSMain PROFILE vs_6_0)

# --- forward ---
# フォワード（不透明 / スキンド / 地形 / グリッド / 派生バリアント）とクラスタードライティング、デカール。-D UNO_PHYSICAL_LIGHTS=1 は Q2 の物理ライティング単位版。
uno_shader(NAME ClusterBuild_CS FILE forward/ClusterBuild.hlsl ENTRY CSMain PROFILE cs_6_0)
uno_shader(NAME ClusterCull_CS FILE forward/ClusterCull.hlsl ENTRY CSMain PROFILE cs_6_0)
uno_shader(NAME ClusterCullDecals_CS FILE forward/ClusterCullDecals.hlsl ENTRY CSMain PROFILE cs_6_0)
uno_shader(NAME Emissive_PS FILE forward/Emissive.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME Emissive_VS FILE forward/Emissive.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME ForwardLdr_PS FILE forward/Forward.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES LDR_OUTPUT=1)
uno_shader(NAME ForwardMaskPhys_PS FILE forward/Forward.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES ALPHA_TEST=1 UNO_PHYSICAL_LIGHTS=1)
uno_shader(NAME ForwardMask_PS FILE forward/Forward.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES ALPHA_TEST=1)
uno_shader(NAME ForwardPhys_PS FILE forward/Forward.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES UNO_PHYSICAL_LIGHTS=1)
uno_shader(NAME Forward_PS FILE forward/Forward.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME Forward_VS FILE forward/Forward.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME ForwardGrid_PS FILE forward/ForwardGrid.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME ForwardGrid_VS FILE forward/ForwardGrid.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME ForwardInstanced_VS FILE forward/ForwardInstanced.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME ForwardSkinned_PS FILE forward/ForwardSkinned.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME ForwardSkinned_VS FILE forward/ForwardSkinned.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME TerrainPhys_PS FILE forward/Terrain.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES UNO_PHYSICAL_LIGHTS=1)
uno_shader(NAME Terrain_PS FILE forward/Terrain.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME Terrain_VS FILE forward/Terrain.hlsl ENTRY VSMain PROFILE vs_6_0)

# --- hiz ---
# Hi-Z オクルージョンカリング。
uno_shader(NAME HiZBuildCopy_CS FILE hiz/HiZBuild.hlsl ENTRY CSCopy PROFILE cs_6_0)
uno_shader(NAME HiZBuildReduce_CS FILE hiz/HiZBuild.hlsl ENTRY CSReduce PROFILE cs_6_0)
uno_shader(NAME HiZCull_CS FILE hiz/HiZCull.hlsl ENTRY CSMain PROFILE cs_6_0)

# --- ibl ---
# IBL（放射照度、プリフィルタ、BRDF LUT、反射プローブ、スカイボックス）。
uno_shader(NAME IntegrateBRDF_CS FILE ibl/IntegrateBRDF.hlsl ENTRY CSMain PROFILE cs_6_0)
uno_shader(NAME IrradianceConvolution_CS FILE ibl/IrradianceConvolution.hlsl ENTRY CSMain PROFILE cs_6_0)
uno_shader(NAME PrefilterEnv_CS FILE ibl/PrefilterEnv.hlsl ENTRY CSMain PROFILE cs_6_0)
uno_shader(NAME ProbeCopyFace_CS FILE ibl/ProbeCopyFace.hlsl ENTRY CSMain PROFILE cs_6_0)
uno_shader(NAME ProbeDownsample_CS FILE ibl/ProbeDownsample.hlsl ENTRY CSMain PROFILE cs_6_0)
uno_shader(NAME Skybox_PS FILE ibl/Skybox.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME Skybox_VS FILE ibl/Skybox.hlsl ENTRY VSMain PROFILE vs_6_0)

# --- instancegpu ---
# GPU 駆動のインスタンス群のカリング。
uno_shader(NAME InstanceCullCount_CS FILE instancegpu/InstanceCull.hlsl ENTRY CSCount PROFILE cs_6_0)
uno_shader(NAME InstanceCullEmit_CS FILE instancegpu/InstanceCull.hlsl ENTRY CSEmit PROFILE cs_6_0)
uno_shader(NAME InstanceCullFinalize_CS FILE instancegpu/InstanceCull.hlsl ENTRY CSFinalize PROFILE cs_6_0)
uno_shader(NAME InstanceCullReset_CS FILE instancegpu/InstanceCull.hlsl ENTRY CSReset PROFILE cs_6_0)
uno_shader(NAME InstanceCullScan_CS FILE instancegpu/InstanceCull.hlsl ENTRY CSScan PROFILE cs_6_0)

# --- particle ---
# パーティクル（CPU / GPU）、トレイル、ビーム。
uno_shader(NAME Beam_PS FILE particle/Beam.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME Beam_VS FILE particle/Beam.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME GpuParticleDraw_VS FILE particle/GpuParticleDraw.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME GpuParticleEmit_CS FILE particle/GpuParticleSim.hlsl ENTRY CSEmit PROFILE cs_6_0)
uno_shader(NAME GpuParticleInit_CS FILE particle/GpuParticleSim.hlsl ENTRY CSInit PROFILE cs_6_0)
uno_shader(NAME GpuParticleKickoff_CS FILE particle/GpuParticleSim.hlsl ENTRY CSKickoff PROFILE cs_6_0)
uno_shader(NAME GpuParticlePrepare_CS FILE particle/GpuParticleSim.hlsl ENTRY CSPrepare PROFILE cs_6_0)
uno_shader(NAME GpuParticleSimulate_CS FILE particle/GpuParticleSim.hlsl ENTRY CSSimulate PROFILE cs_6_0)
uno_shader(NAME ParticleDistort_PS FILE particle/Particle.hlsl ENTRY PSDistort PROFILE ps_6_0)
uno_shader(NAME Particle_PS FILE particle/Particle.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME Particle_VS FILE particle/Particle.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME Trail_PS FILE particle/Trail.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME Trail_VS FILE particle/Trail.hlsl ENTRY VSMain PROFILE vs_6_0)

# --- perception ---
# 知覚層（dx12_perceive）の ID パス。要求があったフレームだけ走る（エディタ専用）。
uno_shader(NAME PerceptionIdSkinned_VS FILE perception/PerceptionId.hlsl ENTRY VSMainSkinned PROFILE vs_6_0)
uno_shader(NAME PerceptionId_PS FILE perception/PerceptionId.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME PerceptionId_VS FILE perception/PerceptionId.hlsl ENTRY VSMain PROFILE vs_6_0)

# --- post ---
# ポストプロセス、露出、被写界深度、モーションブラー、トランジション、TAA、アップスケーラの入力準備。
uno_shader(NAME ExposureAdapt_CS FILE post/AutoExposure.hlsl ENTRY CSAdapt PROFILE cs_6_0)
uno_shader(NAME ExposureHistogram_CS FILE post/AutoExposure.hlsl ENTRY CSHistogram PROFILE cs_6_0)
uno_shader(NAME BloomDown_PS FILE post/Bloom.hlsl ENTRY BloomDownPS PROFILE ps_6_0)
uno_shader(NAME BloomUp_PS FILE post/Bloom.hlsl ENTRY BloomUpPS PROFILE ps_6_0)
uno_shader(NAME Bloom_VS FILE post/Bloom.hlsl ENTRY FSTriVS PROFILE vs_6_0)
uno_shader(NAME DofCoc_PS FILE post/DoF.hlsl ENTRY DofCocPS PROFILE ps_6_0)
uno_shader(NAME DofComposite_PS FILE post/DoF.hlsl ENTRY DofCompositePS PROFILE ps_6_0)
uno_shader(NAME DofGather_PS FILE post/DoF.hlsl ENTRY DofGatherPS PROFILE ps_6_0)
uno_shader(NAME GodRaysBlur_PS FILE post/GodRays.hlsl ENTRY GodRaysBlurPS PROFILE ps_6_0)
uno_shader(NAME GodRaysMask_PS FILE post/GodRays.hlsl ENTRY GodRaysMaskPS PROFILE ps_6_0)
uno_shader(NAME LensFlare_PS FILE post/LensFlare.hlsl ENTRY LensFlarePS PROFILE ps_6_0)
uno_shader(NAME LinearCapture_CS FILE post/LinearCapture.hlsl ENTRY CSMain PROFILE cs_6_0)
uno_shader(NAME LocalExposureDown_CS FILE post/LocalExposure.hlsl ENTRY CSDown PROFILE cs_6_0)
uno_shader(NAME LocalExposureGridBlur_CS FILE post/LocalExposure.hlsl ENTRY CSGridBlur PROFILE cs_6_0)
uno_shader(NAME LocalExposureGrid_CS FILE post/LocalExposure.hlsl ENTRY CSGrid PROFILE cs_6_0)
uno_shader(NAME LocalExposureLumBlur_CS FILE post/LocalExposure.hlsl ENTRY CSLumBlur PROFILE cs_6_0)
uno_shader(NAME LocalExposure_PS FILE post/LocalExposure.hlsl ENTRY LocalExposurePS PROFILE ps_6_0)
uno_shader(NAME MotionBlur_PS FILE post/MotionBlur.hlsl ENTRY MotionBlurPS PROFILE ps_6_0)
uno_shader(NAME PostProcess_PS FILE post/PostProcess.hlsl ENTRY PostPS PROFILE ps_6_0)
uno_shader(NAME PostProcess_VS FILE post/PostProcess.hlsl ENTRY FSTriVS PROFILE vs_6_0)
uno_shader(NAME TaaResolve_PS FILE post/TAA.hlsl ENTRY TaaResolvePS PROFILE ps_6_0)
uno_shader(NAME Transition_PS FILE post/Transition.hlsl ENTRY TransPS PROFILE ps_6_0)
uno_shader(NAME Transition_VS FILE post/Transition.hlsl ENTRY FSTriVS PROFILE vs_6_0)
uno_shader(NAME TransitionPreview_PS FILE post/TransitionPreview.hlsl ENTRY TransPreviewPS PROFILE ps_6_0)
uno_shader(NAME TransitionPreview_VS FILE post/TransitionPreview.hlsl ENTRY FSTriVS PROFILE vs_6_0)
uno_shader(NAME UpscalePrep_CS FILE post/UpscalePrep.hlsl ENTRY UpscalePrepCS PROFILE cs_6_0)

# --- pt ---
# DXR パストレーサー（地上真値）。PathTraceAtmos は -D PT_ATMOSPHERE=1 の別ファイル（従来の DXIL を変えないため）。
uno_shader(NAME PathTraceAtmos_CS FILE pt/PathTrace.hlsl ENTRY PathTraceCS PROFILE cs_6_6 DEFINES PT_ATMOSPHERE=1)
uno_shader(NAME PathTrace_CS FILE pt/PathTrace.hlsl ENTRY PathTraceCS PROFILE cs_6_6)
uno_shader(NAME PtCopy_CS FILE pt/PtCopy.hlsl ENTRY CopyCS PROFILE cs_6_0)

# --- raytracing ---
# DXR 1.1 inline raytracing（RayQuery は SM 6.5 以上。プロファイルは各行の PROFILE を見る）。
uno_shader(NAME RtAlbedo_PS FILE raytracing/RtAlbedo.hlsl ENTRY PSMain PROFILE ps_6_6)
uno_shader(NAME RtAo_PS FILE raytracing/RtAo.hlsl ENTRY PSMain PROFILE ps_6_6)
uno_shader(NAME RtAoDenoise_PS FILE raytracing/RtAoDenoise.hlsl ENTRY PSMain PROFILE ps_6_6)
uno_shader(NAME RtDebug_PS FILE raytracing/RtDebug.hlsl ENTRY PSMain PROFILE ps_6_6)
uno_shader(NAME RtReflComposite_PS FILE raytracing/RtReflection.hlsl ENTRY CompositePS PROFILE ps_6_6)
uno_shader(NAME RtReflSpatial_PS FILE raytracing/RtReflection.hlsl ENTRY SpatialPS PROFILE ps_6_6)
uno_shader(NAME RtReflTemporal_PS FILE raytracing/RtReflection.hlsl ENTRY TemporalPS PROFILE ps_6_6)
uno_shader(NAME RtReflTrace_PS FILE raytracing/RtReflection.hlsl ENTRY TracePS PROFILE ps_6_6)
uno_shader(NAME RtShadowTrace_PS FILE raytracing/RtShadow.hlsl ENTRY PSMain PROFILE ps_6_6 DEFINES RT_SHADOW_RG=1)
uno_shader(NAME RtShadow_PS FILE raytracing/RtShadow.hlsl ENTRY PSMain PROFILE ps_6_6)
uno_shader(NAME Rt_VS FILE raytracing/RtShadow.hlsl ENTRY FSTriVS PROFILE vs_6_0)
uno_shader(NAME RtShadowSpatial_PS FILE raytracing/RtShadowSpatial.hlsl ENTRY PSMain PROFILE ps_6_6)
uno_shader(NAME RtShadowTemporal_PS FILE raytracing/RtShadowTemporal.hlsl ENTRY PSMain PROFILE ps_6_6)

# --- renderdebug ---
# dx12_render_debug（中間バッファ可視化）。
uno_shader(NAME RenderDebug_PS FILE renderdebug/RenderDebug.hlsl ENTRY RenderDebugPS PROFILE ps_6_0)

# --- screenspace ---
# SSR / SSGI（VS は SSR.hlsl の FSTriVS を全パスで共有する）。
uno_shader(NAME ColorDownsample_PS FILE screenspace/SSGI.hlsl ENTRY DownsamplePS PROFILE ps_6_0)
uno_shader(NAME SsgiTemporal_PS FILE screenspace/SSGI.hlsl ENTRY TemporalPS PROFILE ps_6_0)
uno_shader(NAME SsgiTrace_PS FILE screenspace/SSGI.hlsl ENTRY TracePS PROFILE ps_6_0)
uno_shader(NAME SsgiUpsample_PS FILE screenspace/SSGI.hlsl ENTRY UpsamplePS PROFILE ps_6_0)
uno_shader(NAME ScreenSpace_VS FILE screenspace/SSR.hlsl ENTRY FSTriVS PROFILE vs_6_0)
uno_shader(NAME SsrTrace_PS FILE screenspace/SSR.hlsl ENTRY TracePS PROFILE ps_6_0)
uno_shader(NAME SsrUpsample_PS FILE screenspace/SSR.hlsl ENTRY UpsamplePS PROFILE ps_6_0)

# --- shadow ---
# シャドウパス、アルファクリップ（MASK）の深度パス、コンタクトシャドウ、スキンドの深度プリパス。
uno_shader(NAME ContactShadow_PS FILE shadow/ContactShadow.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME ContactShadow_VS FILE shadow/ContactShadow.hlsl ENTRY FSTriVS PROFILE vs_6_0)
uno_shader(NAME DepthPrepassSkinned_VS FILE shadow/DepthPrepassSkinned.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME ShadowMaskInstanced_VS FILE shadow/ShadowMask.hlsl ENTRY VSMainInstanced PROFILE vs_6_0)
uno_shader(NAME ShadowMaskSkinned_VS FILE shadow/ShadowMask.hlsl ENTRY VSMainSkinned PROFILE vs_6_0)
uno_shader(NAME ShadowMask_PS FILE shadow/ShadowMask.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME ShadowMask_VS FILE shadow/ShadowMask.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME ShadowPass_VS FILE shadow/ShadowPass.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME ShadowPassInstanced_VS FILE shadow/ShadowPassInstanced.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME ShadowPassSkinned_VS FILE shadow/ShadowPassSkinned.hlsl ENTRY VSMain PROFILE vs_6_0)

# --- skinning ---
# compute スキニング（スキンドの変形後頂点 → BLAS 用）。
uno_shader(NAME SkinCompute_CS FILE skinning/SkinCompute.hlsl ENTRY CSMain PROFILE cs_6_0)

# --- sprite ---
# スプライト。
uno_shader(NAME Sprite_PS FILE sprite/Sprite.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME Sprite_VS FILE sprite/Sprite.hlsl ENTRY VSMain PROFILE vs_6_0)

# --- ssao ---
# SSAO。
uno_shader(NAME SSAO_PS FILE ssao/SSAO.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME SSAO_VS FILE ssao/SSAO.hlsl ENTRY FSTriVS PROFILE vs_6_0)
uno_shader(NAME SSAOBlur_PS FILE ssao/SSAOBlur.hlsl ENTRY PSMain PROFILE ps_6_0)

# --- velocity ---
# 速度バッファのプリパスとデバッグ。
uno_shader(NAME VelocityDebug_PS FILE velocity/VelocityDebug.hlsl ENTRY VelocityDebugPS PROFILE ps_6_0)
uno_shader(NAME VelocityPrepassMask_PS FILE velocity/VelocityPrepass.hlsl ENTRY VelocityPS PROFILE ps_6_0 DEFINES ALPHA_TEST=1)
uno_shader(NAME VelocityPrepassMask_VS FILE velocity/VelocityPrepass.hlsl ENTRY VSMain PROFILE vs_6_0 DEFINES ALPHA_TEST=1)
uno_shader(NAME VelocityPrepass_PS FILE velocity/VelocityPrepass.hlsl ENTRY VelocityPS PROFILE ps_6_0)
uno_shader(NAME VelocityPrepass_VS FILE velocity/VelocityPrepass.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME VelocityPrepassInstancedMask_VS FILE velocity/VelocityPrepassInstanced.hlsl ENTRY VSMain PROFILE vs_6_0 DEFINES ALPHA_TEST=1)
uno_shader(NAME VelocityPrepassInstanced_VS FILE velocity/VelocityPrepassInstanced.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME VelocityPrepassSkinnedMask_VS FILE velocity/VelocityPrepassSkinned.hlsl ENTRY VSMain PROFILE vs_6_0 DEFINES ALPHA_TEST=1)
uno_shader(NAME VelocityPrepassSkinned_VS FILE velocity/VelocityPrepassSkinned.hlsl ENTRY VSMain PROFILE vs_6_0)

# --- vg ---
# 仮想ジオメトリ（Nanite 風）。cs / ms / as / ps は SM 6.6。専用ルートシグネチャ。VgShared.h は C++ と HLSL の共有定数。
uno_shader(NAME VgClusterCull_CS FILE vg/VgClusterCull.hlsl ENTRY CSClusterCull PROFILE cs_6_6)
uno_shader(NAME VgDebug_PS FILE vg/VgDebug.hlsl ENTRY PSMain PROFILE ps_6_6)
uno_shader(NAME VgDebug_VS FILE vg/VgDebug.hlsl ENTRY VSMain PROFILE vs_6_6)
uno_shader(NAME VgGBuffer_PS FILE vg/VgGBuffer.hlsl ENTRY PSMain PROFILE ps_6_6)
uno_shader(NAME VgGBuffer_VS FILE vg/VgGBuffer.hlsl ENTRY VSMain PROFILE vs_6_6)
uno_shader(NAME VgInstanceCull_CS FILE vg/VgInstanceCull.hlsl ENTRY CSInstanceCull PROFILE cs_6_6)
uno_shader(NAME VgNodeTraverse_CS FILE vg/VgNodeTraverse.hlsl ENTRY CSTraverse PROFILE cs_6_6)
uno_shader(NAME VgRasterAs_MS FILE vg/VgRaster.hlsl ENTRY MSMain PROFILE ms_6_6 DEFINES VG_USE_AS)
uno_shader(NAME VgRasterCount_PS FILE vg/VgRaster.hlsl ENTRY PSMain PROFILE ps_6_6 DEFINES VG_PS_COUNT)
uno_shader(NAME VgRaster_AS FILE vg/VgRaster.hlsl ENTRY ASMain PROFILE as_6_6 DEFINES VG_AS)
uno_shader(NAME VgRaster_MS FILE vg/VgRaster.hlsl ENTRY MSMain PROFILE ms_6_6)
uno_shader(NAME VgRaster_PS FILE vg/VgRaster.hlsl ENTRY PSMain PROFILE ps_6_6)
uno_shader(NAME VgResolve_PS FILE vg/VgResolve.hlsl ENTRY PSMain PROFILE ps_6_6)
uno_shader(NAME VgResolve_VS FILE vg/VgResolve.hlsl ENTRY VSMain PROFILE vs_6_6)
uno_shader(NAME VgArgs_CS FILE vg/VgUtil.hlsl ENTRY CSArgs PROFILE cs_6_6)
uno_shader(NAME VgCoverage_CS FILE vg/VgUtil.hlsl ENTRY CSCoverage PROFILE cs_6_6)
uno_shader(NAME VgRasterArgs_CS FILE vg/VgUtil.hlsl ENTRY CSRasterArgs PROFILE cs_6_6)
uno_shader(NAME VgReset_CS FILE vg/VgUtil.hlsl ENTRY CSReset PROFILE cs_6_6)
uno_shader(NAME VgSortStep_CS FILE vg/VgUtil.hlsl ENTRY CSSortStep PROFILE cs_6_6)

# --- water ---
# 水面（専用ルートシグネチャ）。
uno_shader(NAME WaterSurfacePhys_PS FILE water/WaterSurface.hlsl ENTRY PSMain PROFILE ps_6_0 DEFINES UNO_PHYSICAL_LIGHTS=1)
uno_shader(NAME WaterSurface_PS FILE water/WaterSurface.hlsl ENTRY PSMain PROFILE ps_6_0)
uno_shader(NAME WaterSurface_VS FILE water/WaterSurface.hlsl ENTRY VSMain PROFILE vs_6_0)
uno_shader(NAME WaterOpticsTest_CS FILE water/WaterTest.hlsl ENTRY CSOptics PROFILE cs_6_0)
uno_shader(NAME WaterWaveTest_CS FILE water/WaterTest.hlsl ENTRY CSWave PROFILE cs_6_0)
uno_shader(NAME WaterUnderPhys_PS FILE water/WaterUnder.hlsl ENTRY PSUnder PROFILE ps_6_0 DEFINES UNO_PHYSICAL_LIGHTS=1)
uno_shader(NAME WaterUnder_PS FILE water/WaterUnder.hlsl ENTRY PSUnder PROFILE ps_6_0)
uno_shader(NAME WaterUnder_VS FILE water/WaterUnder.hlsl ENTRY VSFullscreen PROFILE vs_6_0)
