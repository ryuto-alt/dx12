add_library(Graphics STATIC
    GraphicsDevice.cpp
    CommandQueue.cpp
    SwapChain.cpp
    DescriptorHeap.cpp
    FrameResources.cpp
    GpuResource.cpp
    DeferredRelease.cpp
    Buffer.cpp
    RootSignature.cpp
    RenderBackendExt.cpp     # W05-01b: 拡張メンバの入口（空。中身は F1〜F3）
    PipelineState.cpp
    CommandList.cpp
    Texture.cpp
    RenderTarget.cpp
    GpuTimer.cpp
)

target_include_directories(Graphics
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

find_package(directx-headers CONFIG REQUIRED)
find_package(D3D12MemoryAllocator CONFIG REQUIRED)

target_link_libraries(Graphics
    PUBLIC
        Core
        Microsoft::DirectX-Headers
        Microsoft::DirectX-Guids
        GPUOpen::D3D12MemoryAllocator
        d3d12.lib
        dxgi.lib
        dxguid.lib
)

target_compile_options(Graphics PRIVATE ${DX12_COMPILE_OPTIONS})
target_compile_definitions(Graphics PRIVATE ${DX12_COMPILE_DEFS})

# レーン別の追加ソース（中身は各レーンが書く。OPTIONAL は付けない＝読み忘れを検出するため）
include(${CMAKE_SOURCE_DIR}/cmake/lanes/graphics_lane_a.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/graphics_lane_b.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/graphics_lane_c.cmake)
include(${CMAKE_SOURCE_DIR}/cmake/lanes/graphics_lane_d.cmake)
