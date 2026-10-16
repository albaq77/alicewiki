cmake_minimum_required(VERSION 3.16)
project(dbg-viewtype)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_path(LIBDWARF_INCLUDE_DIR libdwarf/libdwarf.h REQUIRED)
find_library(LIBELF_LIBRARY elf REQUIRED)
find_library(LIBDWARF_LIBRARY dwarf REQUIRED)

add_executable(dbg-viewtype dbg-viewtype.cpp)

target_include_directories(dbg-viewtype PRIVATE ${LIBDWARF_INCLUDE_DIR})
target_link_libraries(dbg-viewtype ${LIBELF_LIBRARY} ${LIBDWARF_LIBRARY})
