# Target-based warning configuration. Works with GCC/Clang/MSVC without
# requiring any platform-specific source edits.

function(target_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wconversion -Wsign-conversion
            -Wnull-dereference -Wdouble-promotion)
    endif()
endfunction()

# Softer profile for presentation code (pixel math & layout arithmetic).
function(target_set_warnings_runtime target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnull-dereference)
    endif()
endfunction()

function(target_warnings_off target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W0)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
endfunction()
