# SRC 内の *.dll を DST へコピーする（ビルド時に評価される小さなスクリプト）。
#
# なぜ要るか: vcpkg の applocal デプロイはメインの exe の隣にしか DLL を置かない。
# テスト実行ファイルは build/*/tests/ に出るので、そのままだと spdlog.dll や fmt.dll が
# 見つからず 0xC0000135(STATUS_DLL_NOT_FOUND) で**起動すらできない**。
# ctest からは「失敗」に見えるが実際には 1 行も実行されていない＝コケていることに
# 気付けない、という一番たちの悪い状態になる。
#
# glob は configure 時ではなく実行時に評価する必要がある（ビルドの途中で DLL が
# 増えるため）ので、add_custom_command から -P でこのスクリプトを呼ぶ形にしている。
#
#   cmake -DSRC=<dir> -DDST=<dir> -P tools/copy_runtime_dlls.cmake

if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "copy_runtime_dlls.cmake: SRC と DST の両方が要る")
endif()

file(MAKE_DIRECTORY "${DST}")
file(GLOB _dlls "${SRC}/*.dll")
foreach(_f IN LISTS _dlls)
    file(COPY "${_f}" DESTINATION "${DST}")
endforeach()
