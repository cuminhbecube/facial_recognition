#1.cmake version
cmake_minimum_required(VERSION 3.5)

option(COMPILES_EXAMPLES "Enable compiles examples" OFF)
add_subdirectory(audio)
add_subdirectory(video)
add_subdirectory(misc)
