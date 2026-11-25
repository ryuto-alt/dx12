# シェーダの宣言表を支える関数（MH-0b / W05-16）。ルート CMakeLists.txt の DXC 区画から include される。
#
#   uno_shader(NAME <cso 名（.cso は付けない）> FILE <shaders/ からの相対パス（または絶対パス）>
#              ENTRY <関数> PROFILE <vs_6_0 .. cs_6_9 など>
#              [DEFINES A=1 ...] [DEPENDS <追加の依存ファイル>...] [NEW] [PDB])
#
#   既定のコマンドライン:  dxc -T <PROFILE> -E <ENTRY> [-D <DEFINE>]... -Fo <出力>.cso <FILE>
#   NEW を付けた .cso にだけ `-HV 2021 -WX` を足す（既存の .cso は 1 文字も変えない）。
#   PDB は PIX 用に `-Zi -Qstrip_debug -Fd <NAME>.pdb` を足す（.cso の中身は変えず、.pdb を別に出す）。
#   PROFILE は *_6_9 まで受け付ける（使うのは F 段以降。DXC は baseline のまま動かす）。
#
# 依存（DEPFILE の代わり）:
#   DXC に -MF を渡すとコマンドラインが変わるので使わない。代わりに configure 時に FILE の #include を再帰的に辿り、
#   見つけたファイルをすべて DEPENDS に入れる（dxc の解決規則と同じく、include した側のフォルダ基準）。
#   辿ったファイルは CMAKE_CONFIGURE_DEPENDS に入れるので、#include を足したり消したりして保存すると次のビルドで
#   自動的に再 configure され、依存が張り直される。
#
# 必要な変数（ルート CMakeLists.txt が設定する）:
#   DXC_EXECUTABLE、SHADER_OUTPUT_DIR
# 登録した出力の一覧は、グローバルプロパティ UNO_SHADER_OUTPUTS に溜まる。

set(UNO_SHADER_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/shaders" CACHE INTERNAL "")

# 1 つのファイルが直接 #include している（引用符の）ファイルの絶対パスを返す。結果は同じ configure の中で使い回す。
function(_uno_shader_direct_includes file out)
    string(MD5 _key "${file}")
    get_property(_cached GLOBAL PROPERTY "UNO_INC_CACHE_${_key}" SET)
    if(_cached)
        get_property(_val GLOBAL PROPERTY "UNO_INC_CACHE_${_key}")
        set(${out} "${_val}" PARENT_SCOPE)
        return()
    endif()
    set(_result "")
    if(EXISTS "${file}")
        get_filename_component(_dir "${file}" DIRECTORY)
        file(STRINGS "${file}" _lines ENCODING UTF-8
             REGEX "^[ \t]*#[ \t]*include[ \t]+\"[^\"]+\"")
        foreach(_line IN LISTS _lines)
            string(REGEX REPLACE "^[^\"]*\"([^\"]+)\".*$" "\\1" _inc "${_line}")
            get_filename_component(_abs "${_inc}" ABSOLUTE BASE_DIR "${_dir}")
            if(EXISTS "${_abs}")
                list(APPEND _result "${_abs}")
            else()
                message(WARNING "uno_shader: ${file} の #include \"${_inc}\" が見つかりません（${_abs}）")
            endif()
        endforeach()
    endif()
    set_property(GLOBAL PROPERTY "UNO_INC_CACHE_${_key}" "${_result}")
    set(${out} "${_result}" PARENT_SCOPE)
endfunction()

# src の #include の閉包（src 自身は含まない）。
function(_uno_shader_include_closure src out)
    set(_todo "${src}")
    set(_seen "")
    while(_todo)
        list(POP_FRONT _todo _f)
        if(_f IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_f}")
        _uno_shader_direct_includes("${_f}" _direct)
        list(APPEND _todo ${_direct})
    endwhile()
    list(REMOVE_ITEM _seen "${src}")
    set(${out} "${_seen}" PARENT_SCOPE)
endfunction()

function(uno_shader)
    cmake_parse_arguments(US "NEW;PDB" "NAME;FILE;ENTRY;PROFILE" "DEFINES;DEPENDS" ${ARGN})

    foreach(_req NAME FILE ENTRY PROFILE)
        if(NOT US_${_req})
            message(FATAL_ERROR "uno_shader: ${_req} が必要です（引数: ${ARGN}）")
        endif()
    endforeach()
    if(US_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "uno_shader(${US_NAME}): 不明な引数: ${US_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT US_PROFILE MATCHES "^(vs|ps|cs|ms|as|gs|hs|ds|lib)_6_[0-9]$")
        message(FATAL_ERROR "uno_shader(${US_NAME}): PROFILE の形式が不正です: ${US_PROFILE}（例 ps_6_0。*_6_0 〜 *_6_9）")
    endif()
    if(NOT US_NAME MATCHES "^[A-Za-z0-9_]+$")
        message(FATAL_ERROR "uno_shader: NAME は英数字と _ だけです: ${US_NAME}")
    endif()

    get_property(_names GLOBAL PROPERTY UNO_SHADER_NAMES)
    if(US_NAME IN_LIST _names)
        message(FATAL_ERROR "uno_shader: NAME が重複しています: ${US_NAME}")
    endif()
    set_property(GLOBAL APPEND PROPERTY UNO_SHADER_NAMES "${US_NAME}")

    if(IS_ABSOLUTE "${US_FILE}")
        set(_src "${US_FILE}")
    else()
        set(_src "${UNO_SHADER_SOURCE_ROOT}/${US_FILE}")
    endif()
    if(NOT EXISTS "${_src}")
        message(FATAL_ERROR "uno_shader(${US_NAME}): ソースがありません: ${_src}")
    endif()

    set(_out "${SHADER_OUTPUT_DIR}/${US_NAME}.cso")
    set(_defs "")
    foreach(_d IN LISTS US_DEFINES)
        list(APPEND _defs -D ${_d})
    endforeach()
    set(_extra "")
    if(US_NEW)
        list(APPEND _extra -HV 2021 -WX)
    endif()
    set(_byproducts "")
    if(US_PDB)
        list(APPEND _extra -Zi -Qstrip_debug -Fd "${SHADER_OUTPUT_DIR}/${US_NAME}.pdb")
        set(_byproducts BYPRODUCTS "${SHADER_OUTPUT_DIR}/${US_NAME}.pdb")
    endif()

    _uno_shader_include_closure("${_src}" _incs)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_src}" ${_incs})

    add_custom_command(
        OUTPUT "${_out}"
        COMMAND ${DXC_EXECUTABLE} -T ${US_PROFILE} -E ${US_ENTRY} ${_defs} ${_extra} -Fo "${_out}" "${_src}"
        DEPENDS "${_src}" ${_incs} ${US_DEPENDS}
        ${_byproducts}
        COMMENT "Compiling ${US_FILE} -> ${US_NAME}.cso (${US_PROFILE})"
    )
    set_property(GLOBAL APPEND PROPERTY UNO_SHADER_OUTPUTS "${_out}")
endfunction()
