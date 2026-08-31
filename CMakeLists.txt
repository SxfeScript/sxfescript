cmake_minimum_required(VERSION 3.20)
project(sxn VERSION 0.0.1 LANGUAGES C)
# Every other dependency here (curl, zlib, libuv, libffi) resolves to a
# dylib under /usr/lib on macOS - part of the OS, present on every Mac.
# OpenSSL doesn't ship there anymore, so find_package(OpenSSL) resolves to
# Homebrew's dylib instead, which isn't present on a Mac without Homebrew
# and that exact formula installed. Statically linking it here is what
# actually makes the release binary run on a plain Mac, not just this one.
if(APPLE)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
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

# libffi backs Sxn.ffi. It is a runtime capability rather than a Node one, so
# it is not tied to the node: layer, but it is still optional: a build without
# it keeps Sxn.ffi as a function that explains its own absence.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(LIBFFI QUIET IMPORTED_TARGET libffi)
endif()
if(NOT LIBFFI_FOUND)
  find_path(LIBFFI_INCLUDE_DIR ffi.h PATH_SUFFIXES ffi)
  find_library(LIBFFI_LIBRARY NAMES ffi)
endif()

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

# Both qjsc-generated headers below land in generated/; create it at
# configure time so whichever custom command runs first doesn't fail
# writing to a directory the other one hasn't created yet.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)

# WinterCG globals (TextEncoder, URL, Headers, fetch's JS glue, ...) are
# authored as plain JS in src/bootstrap.js and compiled to bytecode at build
# time, so startup reads a prepared function instead of parsing 140KB of
# JavaScript on every launch -- worth about 3ms of a 10ms cold start.
#
# The compiler is the qjsc built from this same tree, so the bytecode can
# never disagree with the engine that loads it about the format version or
# the atom table. That is the failure the checked-in lazy-builtin blobs are
# exposed to and this is not.
set(SXN_BOOTSTRAP_JS ${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap.js)
set(SXN_BOOTSTRAP_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/sxn_bootstrap.h)
add_custom_command(
  OUTPUT ${SXN_BOOTSTRAP_HEADER}
  COMMAND qjsc -C -s -N sxn_bootstrap_bc -n "<sxn:bootstrap>"
          -o ${SXN_BOOTSTRAP_HEADER} ${SXN_BOOTSTRAP_JS}
  DEPENDS ${SXN_BOOTSTRAP_JS} qjsc
  COMMENT "Compiling bootstrap.js"
  VERBATIM
)
add_custom_target(sxn_bootstrap_header DEPENDS ${SXN_BOOTSTRAP_HEADER})

# node:buffer/path/events/process are authored and compiled the same way
# (src/node_compat.js), kept as a separate file/header from bootstrap.js
# since it's a distinct WinterCG-vs-Node-compat concern.
set(SXN_NODE_COMPAT_JS ${CMAKE_CURRENT_SOURCE_DIR}/src/node_compat.js)
set(SXN_NODE_COMPAT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/sxn_node_compat.h)
add_custom_command(
  OUTPUT ${SXN_NODE_COMPAT_HEADER}
  COMMAND qjsc -m -s -N sxn_node_compat_bc -n "<sxn:node_compat>"
          -o ${SXN_NODE_COMPAT_HEADER} ${SXN_NODE_COMPAT_JS}
  DEPENDS ${SXN_NODE_COMPAT_JS} qjsc
  COMMENT "Compiling node_compat.js"
  VERBATIM
)
add_custom_target(sxn_node_compat_header DEPENDS ${SXN_NODE_COMPAT_HEADER})

add_executable(sxn
  src/main.c
  src/package.c
  src/lsp.c
  src/network.c
  src/node.c
  src/ffi.c
  src/napi.c
)
add_dependencies(sxn sxn_bootstrap_header sxn_node_compat_header)
target_include_directories(sxn PRIVATE include third_party/quickjs third_party/node-api ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(sxn PRIVATE sxfe qjs qjs-libc OpenSSL::Crypto CURL::libcurl libuv::uv_a ZLIB::ZLIB)
if(LIBFFI_FOUND)
  target_link_libraries(sxn PRIVATE PkgConfig::LIBFFI)
elseif(LIBFFI_LIBRARY AND LIBFFI_INCLUDE_DIR)
  target_include_directories(sxn PRIVATE ${LIBFFI_INCLUDE_DIR})
  target_link_libraries(sxn PRIVATE ${LIBFFI_LIBRARY})
else()
  message(FATAL_ERROR "libffi not found; it backs Sxn.ffi (brew install libffi / apt install libffi-dev)")
endif()
if(NOT WIN32)
  target_link_libraries(sxn PRIVATE ${CMAKE_DL_LIBS})
endif()
# A .node addon imports its napi_* functions from whoever loaded it, so this
# executable has to hand its dynamic symbols to dlopen rather than keep them
# private. Without this the addon loads and then cannot find napi_get_cb_info.
set_target_properties(sxn PROPERTIES ENABLE_EXPORTS ON)
if(WIN32)
  # 0x0A00 = Windows 10. GetCurrentThreadStackLimits (src/main.c) needs at
  # least 0x0602 (Windows 8) per mingw-w64's own processthreadsapi.h - 0x0600
  # (Vista) still leaves it undeclared, found by checking that header, not by
  # guessing. Set as a compile definition rather than in-source:
  # third_party/quickjs's cutils.h includes <windows.h> itself, before any
  # #define in main.c would take effect, and windows.h only honors the first
  # inclusion's API level.
  target_compile_definitions(sxn PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  # MinGW's linker default stack reserve is 2MB; sxn_js_stack_budget()
  # (src/main.c) already reserves 2MB of that for native frames beyond the
  # interpreter's own checks, leaving nothing and forcing the 1MB floor.
  # 8MB (glibc/Darwin's usual default, and what that budget was sized
  # against) still wasn't enough here: the same bounded-recursion fixtures
  # that pass comfortably on macOS/Linux need more headroom on Windows,
  # most likely the x64 calling convention's mandatory 32-byte shadow space
  # per call adding up over a few thousand frames. 32MB is a reserve, not a
  # commit - Windows only pages it in as the stack actually grows - so the
  # cost of the difference is address space, not memory.
  target_link_options(sxn PRIVATE -Wl,--stack,33554432)
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
  # The docs site inlines every one of these programs verbatim, so an example
  # that stops running is a broken code block published to the web. fetch.sx
  # is left out on purpose: it is the only one that needs the network.
  foreach(sxn_example hello server files ffi)
    add_test(NAME sxn-example-${sxn_example}
             COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/examples/${sxn_example}.sx)
    set_tests_properties(sxn-example-${sxn_example} PROPERTIES TIMEOUT 30)
  endforeach()
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
  # structuredClone, URL.canParse, crypto.randomUUID, Response statics and
  # clone, Request.clone, Headers.getSetCookie.
  add_test(NAME sxn-web-apis COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/web_apis.mjs)
  # CommonJS: exports, module.exports replacement, JSON, the module cache,
  # a require cycle, and __filename/__dirname.
  add_test(NAME sxn-commonjs COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/cjs/main.cjs)
  # TypeScript erasure: type-only declarations, optional parameters, generics,
  # as/satisfies, and that ordinary `<` comparisons still parse.
  add_test(NAME sxn-typescript COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ts/main.mjs)
  # A .ts file as the entry point, not only as an import.
  add_test(NAME sxn-typescript-entry COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ts/entry.ts)
  # Sxn.serve returns a handle that reports its port and url and can stop.
  # node:util, os, querystring, url and assert, plus process.platform/arch.
  add_test(NAME sxn-node-builtins COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/node_builtins.mjs)
  # File and FormData, the multi-map fetch bodies are built on.
  add_test(NAME sxn-formdata COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/formdata.mjs)
  # fetch with a request body: POST/PUT/PATCH, empty, JSON and large bodies.
  # Web Streams: reading, locking, backpressure, tee, cancel, pipeTo/Through,
  # async iteration, the text transforms, and streams as response bodies.
  # node:stream: flowing and paused modes, pipe, transform, pipeline, the
  # Buffer conversion outside objectMode, and the Web Streams bridges.
  add_test(NAME sxn-node-stream COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/node_stream.mjs)
  set_tests_properties(sxn-node-stream PROPERTIES TIMEOUT 40)
  # node:http: the (req, res) server, headers, chunked writes, request bodies
  # and a response finished after the handler returns.
  # string_decoder, tty, module, timers/promises, the V8 CallSite API that
  # stack-inspecting packages need, and process stdio.
  add_test(NAME sxn-node-misc COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/node_misc.mjs)
  add_test(NAME sxn-node-http COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/node_http.mjs)
  add_test(NAME sxn-node-zlib COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/node_zlib.mjs)
  add_test(NAME sxn-node-crypto-net COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/node_crypto_net.mjs)
  # node:os and the parts of node:fs a server needs, against the real machine
  # rather than against stubbed answers.
  add_test(NAME sxn-node-os-fs COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/node_os_fs.mjs)
  set_tests_properties(sxn-node-os-fs PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
  # node:querystring, now native C, against what Node itself printed.
  add_test(NAME sxn-node-querystring COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/node_querystring.mjs)
  set_tests_properties(sxn-node-querystring PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
  # node:path, posix and win32, against what Node prints for the same corpus.
  add_test(NAME sxn-node-path COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/node_path.mjs)
  set_tests_properties(sxn-node-path PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
  # net.isIP, against Node's own answers.
  add_test(NAME sxn-node-isip COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/node_isip.mjs)
  set_tests_properties(sxn-node-isip PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
  # Every Buffer encoding in both directions, including Node's lenient hex and
  # base64 readers. Expectations are Node's own output, so a divergence fails.
  add_test(NAME sxn-buffer-encodings COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/buffer_encodings.mjs)
  # Node decides module-vs-CommonJS from the nearest package.json's "type", and
  # runs any file it is handed whatever it is called. These three run
  # identically under node, which is the point of them.
  add_test(NAME sxn-entry-commonjs COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/cjsentry/plain.js)
  add_test(NAME sxn-entry-esm-package COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/cjsentry/esmpkg/mod.js)
  add_test(NAME sxn-entry-extensionless COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/cjsentry/tool)
  # Sxn.ffi against the platform's own C library, so there is nothing to build
  # and the functions behave the same on every target.
  add_test(NAME sxn-ffi COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ffi.mjs)
  # sxn compile / sxn --compile-cache / running a .sxbc directly: module and
  # CommonJS bytecode, cache reuse and invalidation, --strip, and the error
  # path for a corrupt file. A shell fixture because it drives sxn itself as
  # a subprocess (compile, then run what it produced) rather than being run
  # by it.
  add_test(NAME sxn-sxbc COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sxbc_roundtrip.sh $<TARGET_FILE:sxn>)
  # A real Node-API addon, compiled here against Node's own headers so the
  # thing under test is the same binary shape npm ships. Its napi_* symbols
  # resolve against whichever process loads it, which ELF/Mach-O allow
  # unresolved at link time (APPLE/UNIX below) but PE does not without an
  # import library sxn.exe doesn't generate yet, so this is skipped on
  # Windows rather than shipping a addon-loading path nothing has verified.
  if(NOT WIN32)
    add_library(sxn_test_addon MODULE ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/napi/hello.c)
    set_target_properties(sxn_test_addon PROPERTIES
      PREFIX "" OUTPUT_NAME "hello" SUFFIX ".node"
      C_VISIBILITY_PRESET default)
    target_include_directories(sxn_test_addon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/node-api)
    target_compile_definitions(sxn_test_addon PRIVATE NODE_GYP_MODULE_NAME=hello)
    if(APPLE)
      target_link_options(sxn_test_addon PRIVATE -undefined dynamic_lookup)
    elseif(UNIX)
      target_link_options(sxn_test_addon PRIVATE -Wl,--unresolved-symbols=ignore-all)
    endif()
    add_test(NAME sxn-napi-addon COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/napi.mjs)
    set_tests_properties(sxn-napi-addon PROPERTIES
      ENVIRONMENT "SXN_TEST_ADDON=$<TARGET_FILE:sxn_test_addon>")
    add_dependencies(sxn sxn_test_addon)
  endif()
  set_tests_properties(sxn-node-http PROPERTIES TIMEOUT 40 FAIL_REGULAR_EXPRESSION "FAIL")
  add_test(NAME sxn-web-streams COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/web_streams.mjs)
  # Crypto/SubtleCrypto brands, and MessageChannel's two ports.
  add_test(NAME sxn-message-channel COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/message_channel.mjs)
  set_tests_properties(sxn-message-channel PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
  set_tests_properties(sxn-web-streams PROPERTIES TIMEOUT 40)
  add_test(NAME sxn-fetch-body COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fetch_body.mjs)
  set_tests_properties(sxn-fetch-body PROPERTIES TIMEOUT 40 FAIL_REGULAR_EXPRESSION "FAIL")
  add_test(NAME sxn-serve-handle COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serve_handle.mjs)
  set_tests_properties(sxn-serve-handle PROPERTIES TIMEOUT 30 FAIL_REGULAR_EXPRESSION "FAIL")
  add_test(NAME sxn-serve-headers COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serve_headers.mjs)
  set_tests_properties(sxn-serve-headers PROPERTIES TIMEOUT 30 FAIL_REGULAR_EXPRESSION "FAIL")
  add_test(NAME sxn-serve-fetch-shape COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serve_fetch_shape.mjs)
  # A request larger than one read: the server has to accumulate reads until
  # the head and the announced body are both in hand.
  add_test(NAME sxn-serve-large-body COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serve_large_body.mjs)
  set_tests_properties(sxn-serve-large-body PROPERTIES TIMEOUT 60 FAIL_REGULAR_EXPRESSION "FAIL")
  # HTTP keep-alive: a connection is reused for the next request, and stop()
  # closes the ones still open.
  add_test(NAME sxn-serve-keepalive COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serve_keepalive.mjs)
  set_tests_properties(sxn-serve-keepalive PROPERTIES TIMEOUT 60 FAIL_REGULAR_EXPRESSION "FAIL")
  # Where a server binds: the hostname option, and reusePort for running one
  # process per core.
  add_test(NAME sxn-serve-bind COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serve_bind.mjs)
  set_tests_properties(sxn-serve-bind PROPERTIES TIMEOUT 60 FAIL_REGULAR_EXPRESSION "FAIL")
  # A served request's headers are built on first read; they still have to
  # behave as if they had been built up front.
  add_test(NAME sxn-serve-lazy-headers COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serve_lazy_headers.mjs)
  set_tests_properties(sxn-serve-lazy-headers PROPERTIES TIMEOUT 60 FAIL_REGULAR_EXPRESSION "FAIL")
  # The request body reaches a handler as the bytes the server read, without
  # a string in between -- including over a reused connection.
  add_test(NAME sxn-serve-body-bytes COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/serve_body_bytes.mjs)
  set_tests_properties(sxn-serve-body-bytes PROPERTIES TIMEOUT 60 FAIL_REGULAR_EXPRESSION "FAIL")
  # JSON.parse/JSON.stringify at the edges of their fast paths: escapes,
  # surrogates, non-ASCII, control characters and every number form. The
  # expectations are Node's own output, so a divergence fails.
  add_test(NAME sxn-json-edges COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/json_edges.mjs)
  set_tests_properties(sxn-json-edges PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
  # 4000 seeded-random documents through parse and stringify, hashed. The
  # expected hash is Node's, so one byte of divergence anywhere fails.
  add_test(NAME sxn-json-fuzz COMMAND sxn ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/json_fuzz.mjs)
  set_tests_properties(sxn-json-fuzz PROPERTIES TIMEOUT 120 FAIL_REGULAR_EXPRESSION "FAIL")
  set_tests_properties(sxn-serve-fetch-shape PROPERTIES TIMEOUT 30 FAIL_REGULAR_EXPRESSION "FAIL")
  set_tests_properties(sxn-performance-now PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
  set_tests_properties(sxn-encode-into PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
endif()

install(TARGETS sxn RUNTIME DESTINATION bin)
