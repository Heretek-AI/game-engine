# EngineHotFlags.cmake — strict per-target compile flags (M1-01).
#
# Usage:
#   engine_apply_hot_flags(<target>)
#     C++20, wall of warnings (+ -Werror unless ENGINE_WERROR=OFF),
#     AVX2 on x86-64 hot modules (guarded: never emitted on ARM/Apple Silicon),
#     -fno-exceptions -fno-rtti (GCC/Clang) — frame path must not unwind.
#
# Non-hot / host-facing modules (bridge host shim, tests) should use
# engine_apply_base_flags(<target>) instead, which keeps exceptions/RTTI.

include(CheckCXXCompilerFlag)

function(engine_apply_base_flags TARGET)
    target_compile_features(${TARGET} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /W4 /permissive- /Zc:__cplusplus)
        if(ENGINE_WERROR)
            target_compile_options(${TARGET} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${TARGET} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wconversion -Wsign-conversion -Wshadow -Wundef
            -fno-strict-aliasing
        )
        if(ENGINE_WERROR)
            target_compile_options(${TARGET} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(engine_apply_hot_flags TARGET)
    engine_apply_base_flags(${TARGET})

    # AVX2 codegen on x86-64 only. aarch64/ARM64 (Apple Silicon, phones) must
    # never see -mavx2; NEON paths are selected per-TU in M1-03.
    if(ENGINE_ENABLE_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|x64)$")
        if(MSVC)
            target_compile_options(${TARGET} PRIVATE /arch:AVX2)
        else()
            target_compile_options(${TARGET} PRIVATE -mavx2 -mfma)
            target_compile_definitions(${TARGET} PUBLIC ENGINE_HAS_AVX2=1)
        endif()
    endif()

    # No unwinding, no RTTI on the frame path (AGENTS.md invariant).
    if(MSVC)
        # /GR- disables RTTI; exceptions stay on under MSVC (required for STL
        # correctness) but engine code must still be noexcept — enforced by
        # review, not by flag, on this toolchain.
        target_compile_options(${TARGET} PRIVATE /GR-)
    else()
        target_compile_options(${TARGET} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
    endif()
endfunction()
