cmake_minimum_required(VERSION 3.16)
project(wayback_dl CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(wayback-dl src/main.cpp)
target_link_libraries(wayback-dl PRIVATE CURL::libcurl Threads::Threads)

if (MSVC)
    target_compile_options(wayback-dl PRIVATE /W4)
else()
    target_compile_options(wayback-dl PRIVATE -Wall -Wextra)
endif()
