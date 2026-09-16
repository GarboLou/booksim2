# BookSim2 as an embeddable static library.
#
# Mirrors src/Makefile, minus main.cpp, so another CMake project can do:
#     add_subdirectory(<path>/booksim2/src booksim)
#     target_link_libraries(my_app booksim)
# The standalone `booksim` binary is still built by the Makefile; this file
# deliberately does not define it, to keep the two paths from fighting over
# object files in the same directory.

cmake_minimum_required(VERSION 3.14)
project(booksim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BOOKSIM_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Same wildcard set the Makefile uses. CONFIGURE_DEPENDS re-globs when files are
# added, so a new network or router is picked up without editing this file.
file(GLOB_RECURSE BOOKSIM_CPP CONFIGURE_DEPENDS
     "${BOOKSIM_SRC_DIR}/*.cpp"
     "${BOOKSIM_SRC_DIR}/*/*.cpp")

# main() belongs to the standalone binary only; the globals it used to define
# now live in booksim_globals.cpp so the library is self-contained without it.
list(REMOVE_ITEM BOOKSIM_CPP "${BOOKSIM_SRC_DIR}/main.cpp")

# tools/*.cpp each define their own main(); they link against this library
# rather than being part of it.
file(GLOB BOOKSIM_TOOLS CONFIGURE_DEPENDS "${BOOKSIM_SRC_DIR}/tools/*.cpp")
if(BOOKSIM_TOOLS)
  list(REMOVE_ITEM BOOKSIM_CPP ${BOOKSIM_TOOLS})
endif()

# The flex/lex and bison/yacc outputs are checked in, so the common case needs
# neither tool. Regenerate only if they are missing.
set(BOOKSIM_LEX  "${BOOKSIM_SRC_DIR}/lex.yy.c")
set(BOOKSIM_YACC "${BOOKSIM_SRC_DIR}/y.tab.c")
set(BOOKSIM_YACC_H "${BOOKSIM_SRC_DIR}/y.tab.h")

if(NOT EXISTS "${BOOKSIM_LEX}" OR NOT EXISTS "${BOOKSIM_YACC}")
  find_package(BISON REQUIRED)
  find_package(FLEX REQUIRED)
  # config.l includes y.tab.h, so yacc must run first.
  add_custom_command(
    OUTPUT "${BOOKSIM_YACC}" "${BOOKSIM_YACC_H}"
    COMMAND ${BISON_EXECUTABLE} -y -d "${BOOKSIM_SRC_DIR}/config.y"
    WORKING_DIRECTORY "${BOOKSIM_SRC_DIR}"
    DEPENDS "${BOOKSIM_SRC_DIR}/config.y"
    COMMENT "bison -y -d config.y")
  add_custom_command(
    OUTPUT "${BOOKSIM_LEX}"
    COMMAND ${FLEX_EXECUTABLE} "${BOOKSIM_SRC_DIR}/config.l"
    WORKING_DIRECTORY "${BOOKSIM_SRC_DIR}"
    DEPENDS "${BOOKSIM_SRC_DIR}/config.l" "${BOOKSIM_YACC_H}"
    COMMENT "flex config.l")
endif()

add_library(booksim STATIC ${BOOKSIM_CPP} ${BOOKSIM_LEX} ${BOOKSIM_YACC})

# The generated parser is C, not C++ -- the Makefile compiles it with $(CC).
set_source_files_properties("${BOOKSIM_LEX}" "${BOOKSIM_YACC}" PROPERTIES LANGUAGE C)

# Linked into the SimAI frontend, which may be a PIE.
set_target_properties(booksim PROPERTIES POSITION_INDEPENDENT_CODE ON)

# PUBLIC so consumers resolve "booksim_engine.hpp" and friends. These mirror
# INCPATH in the Makefile.
target_include_directories(booksim PUBLIC
  "${BOOKSIM_SRC_DIR}"
  "${BOOKSIM_SRC_DIR}/arbiters"
  "${BOOKSIM_SRC_DIR}/allocators"
  "${BOOKSIM_SRC_DIR}/routers"
  "${BOOKSIM_SRC_DIR}/networks"
  "${BOOKSIM_SRC_DIR}/power"
  "${BOOKSIM_SRC_DIR}/engine")

target_compile_options(booksim PRIVATE -Wall -O3 -g)
