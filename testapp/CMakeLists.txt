cmake_minimum_required(VERSION 3.20)
project(mouseapp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include_directories(
  src
  ${CMAKE_SOURCE_DIR}/include
)

# Explicitly list source files to build mouseapp.exe
set(SRC_FILES
  src/mouseapp.cpp
  src/JoyConDecoder.cpp
)

add_executable(mouseapp ${SRC_FILES})

target_link_directories(mouseapp PRIVATE ${CMAKE_SOURCE_DIR}/lib)
target_link_libraries(mouseapp
    PRIVATE
        setupapi
        hid
        ViGEmClient
        windowsapp
)

if(MSVC)
  target_compile_options(mouseapp PRIVATE /W3 /permissive-)
else()
  target_compile_options(mouseapp PRIVATE -Wall -Wextra -pedantic)
endif()