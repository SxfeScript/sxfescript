cmake_minimum_required(VERSION 3.20)
project(sxn VERSION 0.0.1 LANGUAGES C)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
# libuv ships a CMake config package from Homebrew and vcpkg, but Ubuntu's
# libuv1-dev provides only pkg-config, so fall back to that rather than
# requiring the config package everywhere.
find_package(libuv CONFIG QUIET)
if(NOT TARGET libuv::uv_a)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
  add_library(libuv::uv_a ALIAS PkgConfig::LIBUV)
endif()
# New link for Task 5: gzip-decompressing npm tarballs needs zlib's inflate;
# OpenSSL (already linked) has no gzip support. zlib ships with the OS on
# macOS/Linux, so this is a find, not a vendor.
find_package(ZLIB REQUIRED)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Strict -std=c17 hides POSIX declarations on glibc, so a Linux build fails on
# pthread_rwlock_t (via libuv's headers) and PATH_MAX (via limits.h). Ask for
# them explicitly rather than switching to gnu17, so the code itself stays
# standard C. Apple's headers expose both regardless.
if(UNIX AND NOT APPLE)
  add_compile_definitions(_GNU_SOURCE)
endif()

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

# WinterCG globals (TextEncoder, URL, Headers, fetch's JS glue, ...) are
# authored as plain JS in src/bootstrap.js and embedded as a byte array so
# network.c can JS_Eval it at startup without shipping a loose file.
set(SXN_BOOTSTRAP_JS ${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap.js)
set(SXN_BOOTSTRAP_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/sxn_bootstrap.h)
add_custom_command(
  OUTPUT ${SXN_BOOTSTRAP_HEADER}
  COMMAND ${CMAKE_COMMAND} -DIN=${SXN_BOOTSTRAP_JS} -DOUT=${SXN_BOOTSTRAP_HEADER} -DVAR=sxn_bootstrap_js
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_js.cmake
  DEPENDS ${SXN_BOOTSTRAP_JS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_js.cmake
  COMMENT "Embedding bootstrap.js"
)
add_custom_target(sxn_bootstrap_header DEPENDS ${SXN_BOOTSTRAP_HEADER})

# node:buffer/path/events/process are authored the same way (src/node_compat.js
# embedded as a byte array), kept as a separate file/header from bootstrap.js
# since it's a distinct WinterCG-vs-Node-compat concern.
set(SXN_NODE_COMPAT_JS ${CMAKE_CURRENT_SOURCE_DIR}/src/node_compat.js)
set(SXN_NODE_COMPAT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/sxn_node_compat.h)
add_custom_command(
  OUTPUT ${SXN_NODE_COMPAT_HEADER}
  COMMAND ${CMAKE_COMMAND} -DIN=${SXN_NODE_COMPAT_JS} -DOUT=${SXN_NODE_COMPAT_HEADER} -DVAR=sxn_node_compat_js
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_js.cmake
  DEPENDS ${SXN_NODE_COMPAT_JS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_js.cmake
  COMMENT "Embedding node_compat.js"
)
add_custom_target(sxn_node_compat_header DEPENDS ${SXN_NODE_COMPAT_HEADER})

add_executable(sxn
  src/main.c
  src/package.c
  src/lsp.c
  src/network.c
  src/node.c
)
add_dependencies(sxn sxn_bootstrap_header sxn_node_compat_header)
target_include_directories(sxn PRIVATE include third_party/quickjs ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(sxn PRIVATE sxfe qjs qjs-libc OpenSSL::Crypto CURL::libcurl libuv::uv_a ZLIB::ZLIB)
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
  add_test(NAME sxn-memory-report COMMAND sxn --memory-report ${CMAKE_CURRENT_SOURCE_DIR}/examples/velocity.sx)
  add_test(NAME sxn-memory-usage COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/memory-usage.sx)
  add_test(NAME sxn-native-safe COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/compute-native-safe.sx)
  set_tests_properties(sxn-native-safe PROPERTIES ENVIRONMENT "SXN_NATIVE_SX=1")
  add_test(NAME sxn-native-safe-function COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/compute-native-safe-function.sx)
  set_tests_properties(sxn-native-safe-function PROPERTIES ENVIRONMENT "SXN_NATIVE_SX=1")
  add_test(NAME sxn-leak-check COMMAND sxn --leak-check ${CMAKE_CURRENT_SOURCE_DIR}/examples/velocity.sx)
  add_test(NAME sxn-sx-module COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/import-main.sx)
  add_test(NAME sxn-reject-enum COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/reject-enum.sx)
  set_tests_properties(sxn-reject-enum PROPERTIES WILL_FAIL TRUE)
  # Regression coverage: SX's contextual `interface` grammar hook must not
  # leak into ordinary .cjs execution (see update_token_ident in quickjs.c).
  add_test(NAME sxn-interface-ident COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/interface-ident.cjs)
  # TextEncoder.encodeInto spec conformance: fails the run (non-zero exit) if
  # any case deviates, so encoding regressions surface in ctest.
  add_test(NAME sxn-encode-into COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/encodeinto_spec.mjs)
  # Object/property semantics, including the property-less-shape path that
  # skips the per-object property array. Output is diffed against Node in CI
  # by eye; here it must at least run clean.
  add_test(NAME sxn-props-stress COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/props_stress.mjs)
  # Buffer#toString across every encoding, including the mixed-case and
  # latin1/ascii paths that fall through the native dispatch to JS.
  add_test(NAME sxn-buffer-tostring COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/buffer_tostring.mjs)
  add_test(NAME sxn-events-singleton COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/events_singleton.mjs)
  # Constructor prototype resolution, including the memoized slot path:
  # reassignment, interleaved constructors, subclassed builtins,
  # Reflect.construct with an explicit newTarget, Proxy get traps, bound
  # functions and non-object prototypes.
  add_test(NAME sxn-ctor-proto COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ctor_proto.mjs)
  # Accessor semantics: receivers, redefinition, getter<->data transitions,
  # throwing getters, shadowing, frozen objects.
  add_test(NAME sxn-getter-semantics COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/getter_semantics.mjs)
  # Standard-JS integer overflow must promote to double, never wrap. The SX
  # fused accumulator opcode (OP_add_loc_safe_i32) wraps by design, so this
  # guards the boundary: it must stay gated on `safe` locals and never be
  # applied to plain JS/TS accumulators.
  add_test(NAME sxn-js-overflow COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/js_overflow.mjs)
  # Broad standard-JS/TS surface: destructuring, spread, optional chaining,
  # generators, classes (private fields, static blocks), Proxy/Reflect,
  # typed arrays, BigInt, regexp named groups, Map/Set/WeakMap, and the
  # Array/String/Object/Number/JSON builtins. Output is byte-identical to
  # Node; this is the backstop against engine optimizations regressing
  # ordinary JavaScript.
  add_test(NAME sxn-js-conformance COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/js_conformance.mjs)
  # Call paths: native throws, non-callable receivers, stack traces, native
  # callbacks, this-binding, apply/spread/bind, and 1000-deep recursion.
  add_test(NAME sxn-call-edge COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/call_edge.mjs)
  # Stack exhaustion must raise RangeError and stay recoverable -- including
  # when it unwinds through a native frame -- never crash the process.
  add_test(NAME sxn-stack-safety COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/stack_safety.mjs)
  # Temporal dead zone must survive the fused let/const compound-assignment
  # opcodes, and those opcodes must keep exact JS arithmetic (int overflow
  # promotes to double, BigInt and string accumulation still work).
  add_test(NAME sxn-tdz-fusion COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/tdz_fusion.mjs)
  # Mixed int/float arithmetic on the inlined paths: overflow promotion,
  # -0/NaN/Infinity, float+string, and precision at 2^53.
  add_test(NAME sxn-float-math COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/float_math.mjs)
  # Map/Set lookup must stay sublinear, and SameValueZero must survive any
  # change to the key hash. Asserts the shape of the cost curve, not a time.
  add_test(NAME sxn-map-set-hash COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/map_set_hash.mjs)
  set_tests_properties(sxn-map-set-hash PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
  # Array#splice semantics, plus a guard that a removing splice does not
  # convert the array to a slow array (which used to cost ~85x on the next
  # splice and never recovered).
  add_test(NAME sxn-splice COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/splice_conformance.mjs)
  set_tests_properties(sxn-splice PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
  # Array#includes / #indexOf equality semantics, which differ on NaN and
  # must not coerce -- the inlined int fast path has to preserve both.
  add_test(NAME sxn-array-search COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/array_search.mjs)
  # Declaration and scoping rules, which the parser's hash-table lookups must
  # not change: redeclaration errors, block shadowing, TDZ, closure capture
  # per iteration, and the same over 300 declarations so the hashed path runs.
  add_test(NAME sxn-declaration-scoping COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/declaration_scoping.mjs)
  # Regexp semantics across the register-based engine port: groups, named and
  # duplicate-named groups, lookaround, sticky/global state, unicode
  # properties, v flag, modifiers, and a linear-time catastrophic case.
  add_test(NAME sxn-regexp COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/regexp_conformance.mjs)
  # Direct-eval and with-statement scope resolution -- the pseudo-variable
  # paths that the resolver's name indexes must fall back to, never shortcut.
  add_test(NAME sxn-eval-scopes COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/eval_scopes.cjs)
  # Compound-assignment expression values, captured-variable stores, TDZ
  # through closures -- the semantics the checked-store dup elision must keep.
  add_test(NAME sxn-capture-semantics COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/capture_semantics.mjs)
  # ArrayBuffers whose struct and data share one allocation (the encode fast
  # path): views, alignment for Float64, slice, transfer/detach, large
  # buffers, and sustained churn.
  add_test(NAME sxn-embedded-buffer COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/embedded_buffer.mjs)
  # Template literals compile to OP_concat rather than "".concat(...): ToString
  # order, valueOf-vs-toString, throwing parts, symbols, tagged templates.
  add_test(NAME sxn-template-literals COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/template_literals.mjs)
  # `str + int` formats digits straight into the result: aliasing (a shared
  # left operand must not be appended to in place), wide strings, -0, floats,
  # int bounds, and repeated += appends.
  add_test(NAME sxn-string-number-concat COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/string_number_concat.mjs)
  # performance.now bound straight to its C primitive: name, arity,
  # monotonicity, sub-ms resolution, and this-independence.
  add_test(NAME sxn-performance-now COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/performance_now.mjs)
  # Buffer.byteLength across every encoding, views, and the error cases.
  add_test(NAME sxn-buffer-bytelength COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/buffer_bytelength.mjs)
  # The guarded Buffer.from(s,"utf-8").length fusion: fused and unfused results,
  # non-utf-8 encodings, escaping results, and every guard invalidation.
  add_test(NAME sxn-buffer-length-fusion COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/buffer_length_fusion.mjs)
  # The same fusion for encoder.encode(s).length: coercion, surrogates,
  # escaping results, and every guard invalidation.
  add_test(NAME sxn-encode-length-fusion COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/encode_length_fusion.mjs)
  # The guarded emit fusion: numeric promotion, multiple listeners, once,
  # removal during emit, direct _events mutation, and listener `this`.
  add_test(NAME sxn-emit-fusion COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/emit_fusion.mjs)
  # The fusion recognizer shares a switch case with plain calls: nested and
  # computed calls in argument position, and deep recursion that only survives
  # if the tail-call transform still applies.
  add_test(NAME sxn-fusion-call-shapes COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fusion_call_shapes.mjs)
  # Array.fromAsync and Iterator.zip/zipKeyed load from bytecode blobs
  # generated at a pinned BC_VERSION; this catches the blobs going stale
  # against the interpreter, which throws SyntaxError on first access.
  add_test(NAME sxn-lazy-builtins COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/lazy_builtins.mjs)
  # Top-level await has to drive the uv loop, not just the job queue, or an
  # await on a timer, a fetch or the process's own server never resumes.
  add_test(NAME sxn-toplevel-await COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/toplevel_await.mjs)
  set_tests_properties(sxn-toplevel-await PROPERTIES TIMEOUT 30 FAIL_REGULAR_EXPRESSION "FAIL")
  # Sxn.serve response headers, including that a handler cannot corrupt the
  # framing headers that describe the response.
  # Bare specifier resolution over node_modules: plain, scoped, subpath, and
  # nested "exports" conditions, without breaking relative imports.
  add_test(NAME sxn-module-resolution COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/pkgs/resolve.mjs)
  # Event/EventTarget/CustomEvent, which runtime-agnostic packages expect.
  add_test(NAME sxn-event-target COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/event_target.mjs)
  add_test(NAME sxn-serve-headers COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serve_headers.mjs)
  set_tests_properties(sxn-serve-headers PROPERTIES TIMEOUT 30 FAIL_REGULAR_EXPRESSION "FAIL")
  set_tests_properties(sxn-performance-now PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
  set_tests_properties(sxn-encode-into PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
endif()

install(TARGETS sxn RUNTIME DESTINATION bin)
