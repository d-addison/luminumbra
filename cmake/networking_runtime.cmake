include_guard(GLOBAL)

function(luminumbra_collect_gns_runtime_search_dirs)
    # Capture these in the package's directory scope: imported targets need not
    # be GLOBAL, and package variables are local to GNS configuration.
    set(_package_dirs "")
    foreach(_config_var IN ITEMS Protobuf_DIR absl_DIR OpenSSL_DIR)
        if(${_config_var})
            list(APPEND _package_dirs "${${_config_var}}")
        endif()
    endforeach()
    foreach(_target IN ITEMS protobuf::libprotobuf absl::base OpenSSL::Crypto OpenSSL::SSL)
        if(NOT TARGET ${_target})
            continue()
        endif()
        get_target_property(_configs ${_target} IMPORTED_CONFIGURATIONS)
        set(_properties IMPORTED_LOCATION IMPORTED_IMPLIB)
        foreach(_config IN LISTS _configs)
            string(TOUPPER "${_config}" _config)
            list(APPEND _properties "IMPORTED_LOCATION_${_config}" "IMPORTED_IMPLIB_${_config}")
        endforeach()
        foreach(_property IN LISTS _properties)
            get_target_property(_location ${_target} ${_property})
            if(_location)
                get_filename_component(_dir "${_location}" DIRECTORY)
                list(APPEND _package_dirs "${_dir}")
            endif()
        endforeach()
    endforeach()
    set(_search_dirs ${_package_dirs})
    foreach(_dir IN LISTS _package_dirs)
        # Handle both <prefix>/lib/cmake/<package> CONFIG layouts and imported
        # libraries in lib, lib64, bin or a configuration subdirectory thereof.
        get_filename_component(_dir "${_dir}" ABSOLUTE)
        while(TRUE)
            get_filename_component(_part "${_dir}" NAME)
            get_filename_component(_parent "${_dir}" DIRECTORY)
            string(TOLOWER "${_part}" _part)
            if(_part MATCHES "^(bin|lib|lib32|lib64|share)$")
                list(APPEND _search_dirs "${_parent}/bin")
                break()
            endif()
            if(_dir STREQUAL _parent)
                break()
            endif()
            set(_dir "${_parent}")
        endwhile()
    endforeach()
    list(REMOVE_DUPLICATES _search_dirs)
    set_property(TARGET luminumbra_common PROPERTY LUMINUMBRA_NETWORK_RUNTIME_DIRS "${_search_dirs}")
endfunction()

function(luminumbra_require_network_dll OUT_VAR DLL_NAME)
    foreach(_bin IN LISTS ARGN)
        if(EXISTS "${_bin}/${DLL_NAME}" AND NOT IS_DIRECTORY "${_bin}/${DLL_NAME}")
            set(${OUT_VAR} "${_bin}/${DLL_NAME}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "GNS runtime DLL ${DLL_NAME} was not found in the compiler/package bin directories: ${ARGN}")
endfunction()

function(luminumbra_collect_gns_mingw_runtime)
    file(REAL_PATH "${CMAKE_CXX_COMPILER}" _compiler_path)
    get_filename_component(_compiler_bin "${_compiler_path}" DIRECTORY)
    # MSYS2 CONFIG files live in <prefix>/lib/cmake/<package>. Imported targets
    # can omit their DLL locations and dependencies present only in import tables.
    get_filename_component(_protobuf_prefix "${Protobuf_DIR}/../../.." ABSOLUTE)
    get_filename_component(_absl_prefix "${absl_DIR}/../../.." ABSOLUTE)
    get_filename_component(_crypto_lib_dir "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)
    get_filename_component(_crypto_prefix "${_crypto_lib_dir}/.." ABSOLUTE)
    set(_runtime_dlls "")
    foreach(_name IN ITEMS libprotobuf.dll libutf8_validity.dll zlib1.dll)
        luminumbra_require_network_dll(_dll "${_name}"
            "${_protobuf_prefix}/bin" "${_compiler_bin}")
        list(APPEND _runtime_dlls "${_dll}")
    endforeach()
    luminumbra_require_network_dll(_dll libcrypto-3-x64.dll
        "${_crypto_prefix}/bin" "${_compiler_bin}")
    list(APPEND _runtime_dlls "${_dll}")
    set(_absl_dlls "")
    foreach(_bin IN ITEMS "${_absl_prefix}/bin" "${_protobuf_prefix}/bin" "${_compiler_bin}")
        file(GLOB _absl_dlls CONFIGURE_DEPENDS "${_bin}/libabsl_*-2505.0.0.dll")
        if(_absl_dlls)
            break()
        endif()
    endforeach()
    if(NOT _absl_dlls)
        message(FATAL_ERROR "GNS requires MSYS2 abseil runtime DLLs matching libabsl_*-2505.0.0.dll in the protobuf/absl package or compiler bin directory")
    endif()
    list(APPEND _runtime_dlls ${_absl_dlls})
    foreach(_name IN ITEMS libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        luminumbra_require_network_dll(_dll "${_name}" "${_compiler_bin}")
        list(APPEND _runtime_dlls "${_dll}")
    endforeach()
    list(REMOVE_DUPLICATES _runtime_dlls)
    set_property(TARGET luminumbra_common PROPERTY LUMINUMBRA_GNS_RUNTIME_DLLS "${_runtime_dlls}")
endfunction()

function(luminumbra_copy_networking_runtime TARGET_NAME)
    if(WIN32 AND (LUMINUMBRA_ENABLE_GNS OR LUMINUMBRA_ENABLE_STEAM))
        file(REAL_PATH "${CMAKE_CXX_COMPILER}" _compiler_path)
        get_filename_component(_compiler_bin "${_compiler_path}" DIRECTORY)
        set(_search_dirs "${_compiler_bin}")
        set(_extra_dlls "")
        if(LUMINUMBRA_ENABLE_GNS)
            get_target_property(_package_dirs luminumbra_common LUMINUMBRA_NETWORK_RUNTIME_DIRS)
            list(APPEND _search_dirs ${_package_dirs})
            if(MINGW)
                get_target_property(_extra_dlls luminumbra_common LUMINUMBRA_GNS_RUNTIME_DLLS)
            endif()
        else()
            list(APPEND _search_dirs "${LUMINUMBRA_STEAM_SDK_DIR}/redistributable_bin/win64")
            list(APPEND _extra_dlls "$<TARGET_FILE:luminumbra_steam_api>")
        endif()
        list(REMOVE_DUPLICATES _search_dirs)
        set(_target_dlls "")
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
            set(_target_dlls "$<TARGET_RUNTIME_DLLS:${TARGET_NAME}>")
        endif()
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DNETWORK_EXECUTABLE=$<TARGET_FILE:${TARGET_NAME}>"
                "-DNETWORK_RUNTIME_DIR=$<TARGET_FILE_DIR:${TARGET_NAME}>"
                "-DNETWORK_TARGET_DLLS=${_target_dlls}"
                "-DNETWORK_EXTRA_DLLS=${_extra_dlls}"
                "-DNETWORK_SEARCH_DIRS=${_search_dirs}"
                "-DNETWORK_OBJDUMP=${CMAKE_OBJDUMP}"
                "-DNETWORK_DUMPBIN=${CMAKE_DUMPBIN}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/copy_networking_runtime.cmake"
            COMMENT "Scanning and copying networking dependency DLLs next to ${TARGET_NAME}"
            VERBATIM)
    elseif(LUMINUMBRA_ENABLE_STEAM AND UNIX AND NOT APPLE)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LUMINUMBRA_STEAM_SDK_DIR}/redistributable_bin/linux64/libsteam_api.so"
                "$<TARGET_FILE_DIR:${TARGET_NAME}>/libsteam_api.so"
            COMMENT "Copying libsteam_api.so next to ${TARGET_NAME}"
            VERBATIM)
    endif()
endfunction()
