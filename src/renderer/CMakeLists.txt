add_library(Renderer STATIC
    Mesh.cpp
    Camera.cpp
    Material.cpp
    PostProcess.cpp
    BloomPass.cpp
    AutoExposurePass.cpp
    GodRaysPass.cpp
    LensFlarePass.cpp
    DofPass.cpp
    MotionBlurPass.cpp
    SSAOPass.cpp
    ContactShadowPass.cpp
    TaaPass.cpp
    ScreenSpaceGiPass.cpp
    ClusteredLightCulling.cpp
    SpriteRenderer.cpp
    SceneTransition.cpp
    ParticleSystem.cpp
    GpuParticleSystem.cpp
    IBLBaker.cpp
    SkyboxRenderer.cpp
)
target_include_directories(Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)
# meshoptimizer はルート CMakeLists で find_package 済み（POST_BUILD DLL 同梱のため）。
# Resource は ShaderCompiler(.cso ローダ) のために必要。Resource は Renderer に依存しないのでサイクルにならない。
target_link_libraries(Renderer PUBLIC Graphics Microsoft::DirectXMath Resource meshoptimizer::meshoptimizer)

target_compile_options(Renderer PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Renderer PRIVATE ${DX12_COMPILE_DEFS})
