# DlssWiringCheck（W05-07）。引数: -DENGINE_DIR=<DX12Engine.exe のフォルダ> -DCOMPILED_IN_EXE=<DlssCompiledInTests.exe>
#   -DREPO_ROOT=<リポジトリ> [-DPWSH=<pwsh>]
# 1) DlssBackend::CompiledIn()（= get_upscaler の dlss.compiledIn）と exe の隣の nvngx_dlss.dll の有無が一致する。
#    有る → compiledIn=1、無い → compiledIn=0。
# 2) installer/build.ps1 の DLSS 判定: DLL がある → require でも成功。DLL が無い → 既定(auto)は警告つきで成功、require は失敗。
execute_process(COMMAND "${COMPILED_IN_EXE}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "DlssCompiledInTests が失敗 (${_rc}):\n${_out}${_err}")
endif()
if(_out MATCHES "compiledIn=([01])")
    set(_compiled "${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR "compiledIn= の行が無い:\n${_out}")
endif()

if(EXISTS "${ENGINE_DIR}/nvngx_dlss.dll")
    set(_dll 1)
else()
    set(_dll 0)
endif()
if(NOT _dll EQUAL _compiled)
    message(FATAL_ERROR "DLSS の配線が食い違う: compiledIn=${_compiled}、${ENGINE_DIR}/nvngx_dlss.dll の有無=${_dll}（tools/build.ps1 で建て直す）")
endif()
message(STATUS "OK: compiledIn=${_compiled} と nvngx_dlss.dll の有無が一致")
# DLL を配るならライセンス文も隣の licenses/ に要る（DLL とライセンス文は必ずセット）
if(_dll AND NOT EXISTS "${ENGINE_DIR}/licenses/NVIDIA_DLSS_LICENSE.txt")
    message(FATAL_ERROR "nvngx_dlss.dll はあるが ${ENGINE_DIR}/licenses/NVIDIA_DLSS_LICENSE.txt が無い（third_party/dlss/LICENSE.txt を確認して建て直す）")
endif()

if(PWSH)
    set(_ps "${REPO_ROOT}/installer/build.ps1")
    execute_process(COMMAND "${PWSH}" -NoProfile -File "${_ps}" -CheckDlssOnly -SrcDir "${ENGINE_DIR}"
                    RESULT_VARIABLE _auto OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND "${PWSH}" -NoProfile -File "${_ps}" -CheckDlssOnly -Dlss require -SrcDir "${ENGINE_DIR}"
                    RESULT_VARIABLE _req OUTPUT_QUIET ERROR_QUIET)
    if(NOT _auto EQUAL 0)
        message(FATAL_ERROR "installer/build.ps1 の既定 (auto) が DLSS 無しで成功しない (exit ${_auto})")
    endif()
    if(_dll AND NOT _req EQUAL 0)
        message(FATAL_ERROR "DLL があるのに -Dlss require が失敗 (exit ${_req})")
    endif()
    if(NOT _dll AND _req EQUAL 0)
        message(FATAL_ERROR "DLL が無いのに -Dlss require が成功した")
    endif()
    message(STATUS "OK: installer/build.ps1 -Dlss auto/require の挙動")
endif()
