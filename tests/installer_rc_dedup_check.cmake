# installer/dx12engine.iss が、editor.rc で exe に埋め込んだ WAV を assets 側へ二重に同梱しないことを確かめる。
# 使い方: cmake -DREPO_ROOT=<リポジトリ> -P installer_rc_dedup_check.cmake
file(READ "${REPO_ROOT}/installer/dx12engine.iss" _iss)
file(READ "${REPO_ROOT}/resources/editor.rc" _rc)
string(REPLACE "\r" "" _iss "${_iss}")

# assets 全体を入れる Source 行を取り出す（行内に ; があるのでリストにしない。バックスラッシュは [\\] で表す）。
string(REGEX MATCH "Source: \"[{]#RepoRoot[}][\\]assets[\\][*]\"[^\n]*" _assets_line "${_iss}")
if(_assets_line STREQUAL "")
    message(FATAL_ERROR "dx12engine.iss: assets 全体の Source 行が見つからない")
endif()
string(FIND "${_assets_line}" [=[editor\sounds\calibrate\*]=] _p_ex)
string(FIND "${_assets_line}" "Excludes:" _p_kw)
if(_p_kw EQUAL -1 OR _p_ex EQUAL -1)
    message(FATAL_ERROR "dx12engine.iss: assets 配下の Excludes に editor/sounds/calibrate が無い")
endif()

# editor.rc が ../assets/ 配下から RCDATA で埋め込む全ファイルが、除外済みの接頭辞の下にあること。
string(REGEX MATCHALL "RCDATA \"[.][.]/assets/[^\"]+\"" _embedded "${_rc}")
list(LENGTH _embedded _n)
if(_n EQUAL 0)
    message(FATAL_ERROR "editor.rc に assets 配下の RCDATA が見つからない")
endif()
foreach(_e IN LISTS _embedded)
    if(NOT _e MATCHES "RCDATA \"[.][.]/assets/editor/sounds/calibrate/")
        message(FATAL_ERROR "editor.rc が除外対象外の assets を埋め込んでいる（iss/build.ps1 の除外へ追加が必要）: ${_e}")
    endif()
endforeach()

# 配布 zip（installer/build.ps1）も同じ除外をしていること。
file(READ "${REPO_ROOT}/installer/build.ps1" _ps)
string(FIND "${_ps}" [=[Remove-Item -Recurse -Force (Join-Path $stage "assets\editor\sounds\calibrate")]=] _p_ps)
if(_p_ps EQUAL -1)
    message(FATAL_ERROR "installer/build.ps1: zip から assets/editor/sounds/calibrate を除いていない")
endif()

# CMakeLists の OBJECT_DEPENDS 配線。
file(READ "${REPO_ROOT}/CMakeLists.txt" _cm)
string(FIND "${_cm}" "OBJECT_DEPENDS" _p_od)
if(_p_od EQUAL -1)
    message(FATAL_ERROR "CMakeLists.txt: editor.rc の OBJECT_DEPENDS が無い")
endif()
message(STATUS "OK: assets 配下の埋め込み ${_n} 件は iss と build.ps1 で除外済み")
