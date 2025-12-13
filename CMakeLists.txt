cmake_minimum_required(VERSION 3.20)
project(CalcPrice LANGUAGES CXX)

add_executable(CalcPrice WIN32
    main.cpp
    main.rc
    TrayIcon.h
    HotkeyCalc.h
)

target_compile_features(CalcPrice PRIVATE cxx_std_17)
target_compile_definitions(CalcPrice PRIVATE UNICODE _UNICODE)

target_link_libraries(CalcPrice PRIVATE
    user32
    shell32
    comctl32
)

if (MINGW)
    target_link_options(CalcPrice PRIVATE -municode)
endif()