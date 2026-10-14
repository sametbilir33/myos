
cmake_minimum_required(VERSION 3.25)

project(MyOS LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(kernel)
