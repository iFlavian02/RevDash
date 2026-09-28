function(revdash_apply_compiler_options target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_definitions(${target_name} PRIVATE _WIN32_WINNT=0x0A00)
        target_compile_options(${target_name} PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            /EHsc
            /bigobj
            $<$<CONFIG:Debug>:/Od /Zi>
            $<$<CONFIG:Release>:/O2 /DNDEBUG>
        )
        if(REVDASH_ENABLE_ASAN)
            # vcpkg binary dependencies are built without MSVC's optional STL
            # container annotations. Keep the detect_mismatch ABI consistent;
            # ordinary heap/stack/use-after-free instrumentation remains on.
            target_compile_definitions(${target_name} PRIVATE
                _DISABLE_STL_ANNOTATION
                REVDASH_ASAN_ENABLED=1
            )
            target_compile_options(${target_name} PRIVATE /fsanitize=address)

            get_target_property(revdash_target_type ${target_name} TYPE)
            if(revdash_target_type STREQUAL "EXECUTABLE")
                get_filename_component(revdash_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
                add_custom_command(TARGET ${target_name} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${revdash_msvc_bin_dir}/clang_rt.asan_dynamic-x86_64.dll"
                        "$<TARGET_FILE_DIR:${target_name}>"
                    COMMENT "Deploying the MSVC AddressSanitizer runtime"
                    VERBATIM
                )
            endif()
        endif()
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            $<$<CONFIG:Debug>:-O0 -g>
            $<$<CONFIG:Release>:-O3 -DNDEBUG>
        )
        if(REVDASH_ENABLE_ASAN)
            target_compile_options(${target_name} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target_name} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()
