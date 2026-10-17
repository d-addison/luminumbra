# Startup must execute its refusal before entering any common-library code.
# Source options follow Jolt's transitive options. Disable LTO across this
# boundary so optimized engine code cannot be imported ahead of the check.
function(luminumbra_baseline_startup SOURCE_FILE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x86|i[3-6]86)$")
        if(MSVC)
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(_startup_arch "/arch:SSE2")
            else()
                set(_startup_arch "/arch:IA32")
            endif()
            set_source_files_properties(${SOURCE_FILE} PROPERTIES
                COMPILE_OPTIONS "${_startup_arch};/GL-")
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            if(CMAKE_SIZEOF_VOID_P EQUAL 8)
                set(_startup_arch "-march=x86-64")
            else()
                set(_startup_arch "-march=i686;-mno-sse;-mno-sse2")
            endif()
            set_source_files_properties(${SOURCE_FILE} PROPERTIES
                COMPILE_OPTIONS "${_startup_arch};-mno-avx;-mno-avx2;-mno-fma;-mno-bmi;-mno-bmi2;-mno-popcnt;-mno-lzcnt;-mno-f16c;-mno-movbe;-fno-lto")
        endif()
    endif()
endfunction()
