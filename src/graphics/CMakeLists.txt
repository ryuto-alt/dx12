add_library(Graphics STATIC
    GraphicsDevice.cpp
    CommandQueue.cpp
    SwapChain.cpp
    DescriptorHeap.cpp
    FrameResources.cpp
    GpuResource.cpp
    Buffer.cpp
    RootSignature.cpp
    PipelineState.cpp
    CommandList.cpp
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
