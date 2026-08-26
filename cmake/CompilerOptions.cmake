function(revdash_apply_compiler_options target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_20)

    if(MSVC)
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
            target_compile_options(${target_name} PRIVATE /fsanitize=address)
            target_link_options(${target_name} PRIVATE /fsanitize=address)
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
