# D3D12 Agility SDK（retail 1.620.x）の取得・配布の配線。
#
#  - 版・SDK 番号・SHA256 の正は cmake/agility.lock.json の 1 つ。ここにも他のどこにも数字を書かない。
#  - ヘッダは vcpkg の directx-headers（vcpkg.json の overrides で 1.620.2 に固定）。ランタイムの DLL だけを
#    NuGet の Microsoft.Direct3D.D3D12 から configure 時に取る（再頒布ライセンスなのでコミットしない。DLSS と同じ扱い）。
#  - 取得は file(LOCK) で直列化し、一時フォルダへ展開してから rename する。3 つの worktree が同時に
#    configure しても競合しない。取得先は UNO_AGILITY_CACHE_DIR（既定 <repo>/third_party/agility）。
#  - 提供するもの:
#      UNO_AGILITY_SDK / UNO_AGILITY_VERSION / UNO_AGILITY_RUNTIME_DIR（x64 の DLL があるフォルダ）
#      uno_agility_exe(<target>)       exe に D3D12SDKVersion / D3D12SDKPath の export を直接リンクし、
#                                      出力フォルダへ D3D12\ を配る（出力フォルダごとに 1 つのカスタムターゲット）
#      uno_agility_test_exe(<target>)  上に加えて AgilityTestGuard.cpp（Agility のランタイムで走っていないと失敗）
include_guard(GLOBAL)

set(UNO_AGILITY_LOCK_FILE "${CMAKE_CURRENT_LIST_DIR}/agility.lock.json")
set(UNO_AGILITY_COPY_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/UnoAgilityCopy.cmake")
file(READ "${UNO_AGILITY_LOCK_FILE}" _uno_lock_json)
string(JSON UNO_AGILITY_VERSION GET "${_uno_lock_json}" version)
string(JSON UNO_AGILITY_SDK GET "${_uno_lock_json}" sdk)
string(JSON UNO_AGILITY_URL GET "${_uno_lock_json}" source)
string(JSON UNO_AGILITY_SHA256 GET "${_uno_lock_json}" sha256)
string(JSON UNO_AGILITY_CORE_SHA256 GET "${_uno_lock_json}" coreSha256)
string(JSON UNO_AGILITY_LAYERS_SHA256 GET "${_uno_lock_json}" layersSha256)

if(NOT UNO_AGILITY_SDK GREATER_EQUAL 600 OR NOT UNO_AGILITY_SDK LESS 700)
    message(FATAL_ERROR "agility.lock.json の sdk=${UNO_AGILITY_SDK}: プレビュー版（700 以上）は出荷しない（決定 6）")
endif()

set(UNO_AGILITY_CACHE_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party/agility" CACHE PATH
    "Agility SDK ランタイムの取得先（gitignore。コミットしない）")
get_filename_component(UNO_AGILITY_CACHE_DIR "${UNO_AGILITY_CACHE_DIR}" ABSOLUTE)
set(UNO_AGILITY_RUNTIME_DIR "${UNO_AGILITY_CACHE_DIR}/${UNO_AGILITY_VERSION}/x64")
set(UNO_AGILITY_LICENSE_DIR "${UNO_AGILITY_CACHE_DIR}/${UNO_AGILITY_VERSION}")

# --- 取得 -------------------------------------------------------------------------------
function(_uno_agility_have result)
    if(EXISTS "${UNO_AGILITY_RUNTIME_DIR}/D3D12Core.dll" AND EXISTS "${UNO_AGILITY_RUNTIME_DIR}/d3d12SDKLayers.dll"
       AND EXISTS "${UNO_AGILITY_LICENSE_DIR}/LICENSE.txt")
        set(${result} TRUE PARENT_SCOPE)
    else()
        set(${result} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_uno_agility_fetch)
    _uno_agility_have(_have)
    if(_have)
        return()
    endif()
    file(MAKE_DIRECTORY "${UNO_AGILITY_CACHE_DIR}")
    file(LOCK "${UNO_AGILITY_CACHE_DIR}/.fetch.lock" GUARD FUNCTION TIMEOUT 900 RESULT_VARIABLE _lk)
    if(NOT _lk STREQUAL "0")
        message(FATAL_ERROR "Agility: 取得ロックを待ちきれなかった（${_lk}）: ${UNO_AGILITY_CACHE_DIR}/.fetch.lock")
    endif()
    # ロックを取っている間に別の configure が済ませたかもしれない
    _uno_agility_have(_have)
    if(_have)
        return()
    endif()

    set(_pkg "${UNO_AGILITY_CACHE_DIR}/Microsoft.Direct3D.D3D12.${UNO_AGILITY_VERSION}.nupkg")
    message(STATUS "Agility: ${UNO_AGILITY_VERSION} を取得（${UNO_AGILITY_URL}）")
    file(DOWNLOAD "${UNO_AGILITY_URL}" "${_pkg}"
         EXPECTED_HASH SHA256=${UNO_AGILITY_SHA256}
         STATUS _st TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        file(REMOVE "${_pkg}")
        message(FATAL_ERROR "Agility: ダウンロードに失敗または SHA256 が不一致: ${_st}")
    endif()

    # 展開に失敗して残った一時フォルダを（ロックの中で）掃除する。
    file(GLOB _stale LIST_DIRECTORIES true "${UNO_AGILITY_CACHE_DIR}/.tmp-*")
    foreach(_s IN LISTS _stale)
        file(REMOVE_RECURSE "${_s}")
    endforeach()
    string(RANDOM LENGTH 8 _rnd)
    set(_tmp "${UNO_AGILITY_CACHE_DIR}/.tmp-${_rnd}")
    file(REMOVE_RECURSE "${_tmp}")
    file(MAKE_DIRECTORY "${_tmp}")
    file(ARCHIVE_EXTRACT INPUT "${_pkg}" DESTINATION "${_tmp}"
         PATTERNS "build/native/bin/x64/D3D12Core.dll" "build/native/bin/x64/d3d12SDKLayers.dll"
                  "LICENSE.txt" "LICENSE-CODE.txt")
    set(_stage "${_tmp}/stage/${UNO_AGILITY_VERSION}")
    file(MAKE_DIRECTORY "${_stage}/x64")
    foreach(_n D3D12Core.dll d3d12SDKLayers.dll)
        if(NOT EXISTS "${_tmp}/build/native/bin/x64/${_n}")
            file(REMOVE_RECURSE "${_tmp}")
            message(FATAL_ERROR "Agility: nupkg に ${_n} が無い")
        endif()
        file(RENAME "${_tmp}/build/native/bin/x64/${_n}" "${_stage}/x64/${_n}")
    endforeach()
    foreach(_n LICENSE.txt LICENSE-CODE.txt)
        if(EXISTS "${_tmp}/${_n}")
            file(RENAME "${_tmp}/${_n}" "${_stage}/${_n}")
        endif()
    endforeach()
    file(SHA256 "${_stage}/x64/D3D12Core.dll" _core)
    file(SHA256 "${_stage}/x64/d3d12SDKLayers.dll" _layers)
    if(NOT _core STREQUAL UNO_AGILITY_CORE_SHA256 OR NOT _layers STREQUAL UNO_AGILITY_LAYERS_SHA256)
        file(REMOVE_RECURSE "${_tmp}")
        message(FATAL_ERROR "Agility: 展開した DLL の SHA256 が lock と違う（core=${_core} layers=${_layers}）")
    endif()
    # 最後に rename（取りかけの状態を他の configure に見せない）。既存（不完全）があれば消してから。
    file(REMOVE_RECURSE "${UNO_AGILITY_CACHE_DIR}/${UNO_AGILITY_VERSION}")
    file(RENAME "${_stage}" "${UNO_AGILITY_CACHE_DIR}/${UNO_AGILITY_VERSION}")
    file(REMOVE_RECURSE "${_tmp}")
endfunction()
_uno_agility_fetch()

# 取得済みのファイルが lock と違えば（途中で壊れた・差し替わった）止める
file(SHA256 "${UNO_AGILITY_RUNTIME_DIR}/D3D12Core.dll" _uno_core_now)
if(NOT _uno_core_now STREQUAL UNO_AGILITY_CORE_SHA256)
    message(FATAL_ERROR "Agility: ${UNO_AGILITY_RUNTIME_DIR}/D3D12Core.dll の SHA256 が lock と違う。"
                        "そのフォルダを消して configure をやり直す")
endif()
message(STATUS "Agility: ${UNO_AGILITY_VERSION}（SDK ${UNO_AGILITY_SDK}）${UNO_AGILITY_RUNTIME_DIR}")

# tools/fetch_agility.ps1 から  cmake -DUNO_AGILITY_FETCH_ONLY=ON -P cmake/UnoAgility.cmake  で呼ぶと、取得と検証だけして終わる。
if(UNO_AGILITY_FETCH_ONLY)
    return()
endif()

# lock から生成するヘッダ（内容が変わったときだけ書き換わる）
set(UNO_AGILITY_GEN_DIR "${CMAKE_BINARY_DIR}/generated/agility")
configure_file("${CMAKE_CURRENT_LIST_DIR}/UnoAgilityLock.h.in" "${UNO_AGILITY_GEN_DIR}/UnoAgilityLock.h" @ONLY)

get_filename_component(UNO_AGILITY_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/graphics" ABSOLUTE)

# 非 GLOBAL の IMPORTED ターゲットは find_package したディレクトリ以下でしか見えないので、ルートで探す。
find_package(directx-headers CONFIG REQUIRED)

# --- exe へ配線 -------------------------------------------------------------------------------
function(uno_agility_exe target)
    target_sources(${target} PRIVATE "${UNO_AGILITY_SRC_DIR}/AgilityExports.cpp")
    target_include_directories(${target} PRIVATE "${UNO_AGILITY_GEN_DIR}")
    target_link_libraries(${target} PRIVATE Microsoft::DirectX-Headers)

    # 出力フォルダごとに 1 つのカスタムターゲット（12 本の POST_BUILD が同じ D3D12\D3D12Core.dll へ
    # 並列にコピーして共有違反になるのを防ぐ）。
    get_target_property(_outdir ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT _outdir)
        get_target_property(_outdir ${target} BINARY_DIR)
    endif()
    string(MD5 _key "${_outdir}")
    string(SUBSTRING "${_key}" 0 8 _key)
    set(_rt uno_agility_runtime_${_key})
    if(NOT TARGET ${_rt})
        add_custom_target(${_rt}
            COMMAND ${CMAKE_COMMAND}
                    -DSRC=${UNO_AGILITY_RUNTIME_DIR}
                    -DDST=$<TARGET_FILE_DIR:${target}>/D3D12
                    -P ${UNO_AGILITY_COPY_SCRIPT}
            COMMENT "Agility ${UNO_AGILITY_VERSION} -> <exe>/D3D12 (${_key})"
            VERBATIM)
    endif()
    add_dependencies(${target} ${_rt})
endfunction()

function(uno_agility_test_exe target)
    uno_agility_exe(${target})
    target_sources(${target} PRIVATE "${UNO_AGILITY_SRC_DIR}/AgilityTestGuard.cpp")
endfunction()
