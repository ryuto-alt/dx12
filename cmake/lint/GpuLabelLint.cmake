# ctest GpuLabelLint: デバイスを作るテストに gpu ラベルが付いていることを確かめる。
# 検査はコンフィグ時に tests/CMakeLists.txt の dx12_gpu_label_lint_collect() が行い、
# 結果（違反するテスト名の一覧）をファイルへ書く。ここではそれを読んで落とすだけ。
# 使い方: cmake -DREPORT=<file> -P GpuLabelLint.cmake
if(NOT REPORT OR NOT EXISTS "${REPORT}")
    message(FATAL_ERROR "GpuLabelLint: レポートが無い: ${REPORT}")
endif()
file(READ "${REPORT}" _r)
string(STRIP "${_r}" _r)
if(_r)
    message(FATAL_ERROR "GpuLabelLint: D3D12CreateDevice / GraphicsDevice を使うのに gpu ラベルが無いテスト:\n${_r}\n(tests/CMakeLists.txt の set_tests_properties で LABELS gpu を付ける。dxc も要るなら \"gpu;dxc\")")
endif()
message(STATUS "GpuLabelLint: OK")
