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

# editor.rc が assets/editor/sounds/calibrate/ の WAV を RCDATA で埋め込んでいること（除外の根拠）。
string(REGEX MATCHALL "RCDATA \"[.][.]/assets/editor/sounds/calibrate/[^\"]+\"" _embedded "${_rc}")
list(LENGTH _embedded _n)
if(_n EQUAL 0)
    message(FATAL_ERROR "editor.rc に calibrate の RCDATA が見つからない")
endif()
message(STATUS "OK: calibrate の埋め込み ${_n} 件は iss で除外済み")
