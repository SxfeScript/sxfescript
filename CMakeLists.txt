cmake_minimum_required(VERSION 3.20)
project(sxn VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(SXN_ENABLE_POISON "Poison moved and released arena storage" ON)
option(SXN_BUILD_TESTS "Build SXN tests" ON)

set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)

add_library(sxfe STATIC
  src/frontend.c
  src/arena.c
)
target_include_directories(sxfe PUBLIC include)
if(SXN_ENABLE_POISON)
  target_compile_definitions(sxfe PUBLIC SXN_ENABLE_POISON=1)
endif()

add_executable(sxn
  src/main.c
  src/package.c
  src/lsp.c
)
target_include_directories(sxn PRIVATE include third_party/quickjs)
target_link_libraries(sxn PRIVATE sxfe qjs qjs-libc)
if(WIN32)
  target_compile_definitions(sxn PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
endif()

include(CTest)
if(BUILD_TESTING AND SXN_BUILD_TESTS)
  add_executable(sxfe-tests tests/frontend_test.c)
  target_link_libraries(sxfe-tests PRIVATE sxfe)
  if(MSVC)
    target_compile_options(sxfe-tests PRIVATE /UNDEBUG)
  else()
    target_compile_options(sxfe-tests PRIVATE -UNDEBUG)
  endif()
  add_test(NAME sxfe-frontend COMMAND sxfe-tests)
  add_test(NAME sxn-help COMMAND sxn --help)
  add_test(NAME sxn-example COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/examples/velocity.sx)
  add_test(NAME sxn-sx-module COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/import-main.sx)
  add_test(NAME sxn-reject-enum COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/reject-enum.sx)
  set_tests_properties(sxn-reject-enum PROPERTIES WILL_FAIL TRUE)
endif()

install(TARGETS sxn RUNTIME DESTINATION bin)
