cmake_minimum_required(VERSION 3.16)
project(work4-tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)
add_compile_options(-Wall -Wextra -O2)

add_executable(work4-tools WIN32
  src/main.cpp
  src/ui.cpp
  src/index.cpp
  src/search.cpp
  src/worker.cpp
  src/utils.cpp
  resources/app.rc
)

target_link_libraries(work4-tools PRIVATE
  comctl32
  shell32
  user32
  gdi32
  advapi32
  uxtheme
  dwmapi
)

target_link_options(work4-tools PRIVATE -static -static-libgcc -static-libstdc++ -municode)
