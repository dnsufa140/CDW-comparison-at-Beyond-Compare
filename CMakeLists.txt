cmake_minimum_required(VERSION 3.16)
project(cdw2png VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(cdw2png main.cpp)

target_compile_definitions(cdw2png PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(cdw2png PRIVATE ole32 oleaut32 uuid shell32)

if(MSVC)
    # Static CRT (/MT) so the resulting .exe does not require vcruntime140.dll
    set_property(TARGET cdw2png PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(cdw2png PRIVATE /W4 /O2 /utf-8)
elseif(MINGW)
    target_compile_options(cdw2png PRIVATE -Wall -Wextra -O2 -ffunction-sections -fdata-sections)
    target_link_options(cdw2png PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s)
endif()
