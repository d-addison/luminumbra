# Keep one shared GCC runtime across the public C++ DLL boundary. Resolve it
# through this build's compiler directory, never the ambient PATH.
function(luminumbra_install_static_preview_mingw_runtime)
    if(NOT MINGW)
        return()
    endif()
    file(REAL_PATH "${CMAKE_CXX_COMPILER}" compiler_path)
    get_filename_component(compiler_bin "${compiler_path}" DIRECTORY)
    set(runtime_files "")
    foreach(name IN ITEMS libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${name}"
            RESULT_VARIABLE result OUTPUT_VARIABLE resolved ERROR_VARIABLE diagnostic
            OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 15)
        # MSYS2 GCC can return the unchanged name: runtime DLLs in bin are not
        # linker inputs in its library search directories. Admit that exact
        # response only by selecting the named file beside the real compiler.
        # Other relative paths and failed lookups never trigger this rule.
        if(result STREQUAL "0" AND resolved STREQUAL name)
            set(resolved "${compiler_bin}/${name}")
        endif()
        if(NOT result STREQUAL "0" OR resolved MATCHES "[;\r\n]" OR
           NOT IS_ABSOLUTE "${resolved}" OR NOT EXISTS "${resolved}" OR
           IS_DIRECTORY "${resolved}" OR IS_SYMLINK "${resolved}")
            message(FATAL_ERROR "StaticPreview cannot resolve ${name} from the configured MinGW compiler: ${resolved} ${diagnostic}")
        endif()
        file(REAL_PATH "${resolved}" runtime_path)
        get_filename_component(runtime_bin "${runtime_path}" DIRECTORY)
        get_filename_component(runtime_name "${runtime_path}" NAME)
        set(expected_bin "${compiler_bin}")
        if(WIN32)
            string(TOLOWER "${runtime_bin}" runtime_bin)
            string(TOLOWER "${expected_bin}" expected_bin)
        endif()
        if(NOT runtime_bin STREQUAL expected_bin OR NOT runtime_name STREQUAL name)
            message(FATAL_ERROR "StaticPreview ${name} must resolve beside the configured MinGW compiler: ${runtime_path}")
        endif()
        list(APPEND runtime_files "${runtime_path}")
    endforeach()
    install(FILES ${runtime_files} DESTINATION bin COMPONENT StaticPreview)

    # UCRT64 redistributable runtime notices belong to the same toolchain as the DLLs.
    get_filename_component(toolchain_root "${compiler_bin}" DIRECTORY)
    foreach(member IN ITEMS gcc-libs/README gcc-libs/COPYING3 gcc-libs/COPYING.LIB
                            gcc-libs/COPYING.RUNTIME winpthreads/COPYING)
        set(notice "${toolchain_root}/share/licenses/${member}")
        if(NOT EXISTS "${notice}" OR IS_DIRECTORY "${notice}" OR IS_SYMLINK "${notice}")
            message(FATAL_ERROR "StaticPreview MinGW runtime notice is missing: ${notice}")
        endif()
        get_filename_component(family "${member}" DIRECTORY)
        install(FILES "${notice}" DESTINATION "share/luminumbra-static/licenses/${family}"
            COMPONENT StaticPreview)
    endforeach()
endfunction()
