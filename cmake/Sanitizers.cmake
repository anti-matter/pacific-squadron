# Optional sanitizer support for development builds:
#   cmake -S . -B build -DWWII_ENABLE_SANITIZERS=address
option(WWII_ENABLE_SANITIZERS "Enable sanitizers (address, undefined, thread)" "")

if(WWII_ENABLE_SANITIZERS AND NOT MSVC)
    set(_flags "-fsanitize=${WWII_ENABLE_SANITIZERS} -fno-omit-frame-pointer")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_flags}" PARENT_SCOPE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_flags}" PARENT_SCOPE)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_flags}" PARENT_SCOPE)
endif()
