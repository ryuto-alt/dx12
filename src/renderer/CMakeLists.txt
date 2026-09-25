add_library(Renderer STATIC
    Mesh.cpp
    Camera.cpp
    Material.cpp
    PostProcess.cpp
    ScreenShaderPass.cpp
    BloomPass.cpp
    AutoExposurePass.cpp
    GodRaysPass.cpp
    LensFlarePass.cpp
    DofPass.cpp
    MotionBlurPass.cpp
    SSAOPass.cpp
    ContactShadowPass.cpp
    TaaPass.cpp
    RenderDebugPass.cpp
    ScreenSpaceGiPass.cpp
    AccelerationStructure.cpp
    RaytracingScene.cpp
    RtScreenPass.cpp
    SkinningCompute.cpp
    DdgiVolume.cpp
    ClusteredLightCulling.cpp
    HiZPass.cpp
    OcclusionCullPass.cpp
    VolumetricFogPass.cpp
    DecalSystem.cpp
    SpriteRenderer.cpp
    SceneTransition.cpp
    ParticleSystem.cpp
    GpuParticleSystem.cpp
    IBLBaker.cpp
    SkyboxRenderer.cpp
    ViewPasses.cpp          # RenderView の段を IRenderPass（RenderPass.h の状態の契約）として切り出したもの
    PerceptionPass.cpp      # 知覚層（dx12_perceive）の ID パス + 読み戻し
    PerceptionStats.cpp     # 知覚層の集計（GPU 非依存の純関数。tests/perception_stats_test.cpp）
)
target_include_directories(Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)
# meshoptimizer はルート CMakeLists で find_package 済み（POST_BUILD DLL 同梱のため）。
# Resource は ShaderCompiler(.cso ローダ) のために必要。Resource は Renderer に依存しないのでサイクルにならない。
target_link_libraries(Renderer PUBLIC Graphics Microsoft::DirectXMath Resource meshoptimizer::meshoptimizer)

target_compile_options(Renderer PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Renderer PRIVATE ${DX12_COMPILE_DEFS})
