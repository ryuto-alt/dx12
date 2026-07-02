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
    SpriteRenderer.cpp
    SceneTransition.cpp
    ParticleSystem.cpp
    GpuParticleSystem.cpp
    IBLBaker.cpp
    SkyboxRenderer.cpp
)
target_include_directories(Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(directxmath CONFIG REQUIRED)
# Resource は ShaderCompiler(.cso ローダ) のために必要。Resource は Renderer に依存しないのでサイクルにならない。
target_link_libraries(Renderer PUBLIC Graphics Microsoft::DirectXMath Resource)

target_compile_options(Renderer PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Renderer PRIVATE ${DX12_COMPILE_DEFS})
