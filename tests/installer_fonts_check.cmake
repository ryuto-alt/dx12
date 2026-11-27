# W1-A14: UI 書体（IBM Plex / PlemolJP / Noto Sans JP のサブセット）が、インストーラ・配布 zip・POST_BUILD のコピーに入っていること。
# 使い方: cmake -DREPO_ROOT=<リポジトリ> -P installer_fonts_check.cmake
file(READ "${REPO_ROOT}/installer/dx12engine.iss" _iss)
string(REPLACE "\r" "" _iss "${_iss}")
file(READ "${REPO_ROOT}/installer/build.ps1" _ps)
file(READ "${REPO_ROOT}/CMakeLists.txt" _cm)

foreach(_g ibm-plex plemoljp noto-sans-jp)
    # コメント行（; 始まり）ではない Source 行に、書体のサブセットを assets\editor\fonts\ui へ入れる記述があること。
    string(CONCAT _needle
        [=[Source: "{#RepoRoot}\third_party\fonts\]=] "${_g}"
        [=[\subset\*.ttf"; DestDir: "{app}\assets\editor\fonts\ui"]=])
    string(FIND "\n${_iss}" "\n${_needle}" _m)
    if(_m EQUAL -1)
        message(FATAL_ERROR "dx12engine.iss: ${_g} の書体を assets/editor/fonts/ui へ入れる Source 行が無い")
    endif()
    # 実ファイルが 1 つ以上あること
    file(GLOB _ttf "${REPO_ROOT}/third_party/fonts/${_g}/subset/*.ttf")
    list(LENGTH _ttf _n)
    if(_n EQUAL 0)
        message(FATAL_ERROR "third_party/fonts/${_g}/subset/ に .ttf が無い")
    endif()
    string(FIND "${_ps}" "\"${_g}\"" _p)
    if(_p EQUAL -1)
        message(FATAL_ERROR "installer/build.ps1: 配布 zip へ ${_g} の書体を入れていない")
    endif()
endforeach()
string(FIND "${_ps}" [=[assets\editor\fonts\ui]=] _p)
if(_p EQUAL -1)
    message(FATAL_ERROR "installer/build.ps1: zip の assets/editor/fonts/ui への配置が無い")
endif()
string(FIND "${_cm}" "assets/editor/fonts/ui" _p)
if(_p EQUAL -1)
    message(FATAL_ERROR "CMakeLists.txt: 書体の POST_BUILD コピー（assets/editor/fonts/ui）が無い")
endif()
message(STATUS "OK: UI 書体 3 組がインストーラ・zip・POST_BUILD に入っている")
