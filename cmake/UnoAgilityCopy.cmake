# Agility のランタイムを <exe>/D3D12/ へ配る（ビルド時に -P で評価される）。
#   cmake -DSRC=<取得先 x64 フォルダ> -DDST=<exe の隣>/D3D12 -P cmake/UnoAgilityCopy.cmake
# D3D12Core.dll と d3d12SDKLayers.dll の両方を置く（Release でも DX12_D3D_DEBUG=1 でデバッグレイヤーを使うため）。
# 配布の 3 経路（BuildGame / インストーラ / 更新 zip）は D3D12Core.dll だけを運ぶ。
if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "UnoAgilityCopy.cmake: SRC と DST の両方が要る")
endif()
file(MAKE_DIRECTORY "${DST}")
foreach(_n D3D12Core.dll d3d12SDKLayers.dll)
    if(NOT EXISTS "${SRC}/${_n}")
        message(FATAL_ERROR "UnoAgilityCopy.cmake: ${SRC}/${_n} が無い（configure をやり直す）")
    endif()
    file(COPY_FILE "${SRC}/${_n}" "${DST}/${_n}" ONLY_IF_DIFFERENT RESULT _r)
    if(NOT _r STREQUAL "0")
        message(FATAL_ERROR "UnoAgilityCopy.cmake: ${_n} のコピーに失敗: ${_r}")
    endif()
endforeach()
# 再配布条件のライセンス文（取得先の 1 つ上）。ゲームの配布（BuildGame）が licenses/ へ運ぶ元になる。
if(EXISTS "${SRC}/../LICENSE.txt")
    file(COPY_FILE "${SRC}/../LICENSE.txt" "${DST}/LICENSE.txt" ONLY_IF_DIFFERENT)
endif()
