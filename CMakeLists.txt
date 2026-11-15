cmake_minimum_required(VERSION 3.22)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
project(circud VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

option(CIRCUD_ENABLE_ASAN "Build circud and libcjs with AddressSanitizer" OFF)
option(CIRCUD_ENABLE_STRICT_WARNINGS
       "Treat warnings in circud sources as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# libcjs enables CMake IPO for non-Debug builds. CMake maps GCC IPO to
# -flto=auto, which may fan one nominally serial Ninja link out across every
# CPU and exhaust small development/OpenWrt builders. Keep LTO enabled while
# making its internal worker count match circud's deliberately small builds.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_C_COMPILE_OPTIONS_IPO -flto=1 -fno-fat-lto-objects)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_COMPILE_OPTIONS_IPO -flto=1 -fno-fat-lto-objects)
endif()

include(CTest)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Boost 1.75 REQUIRED COMPONENTS json)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MINIZIP REQUIRED IMPORTED_TARGET minizip)

set(CIRCUD_LIBCJS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../cno-cli/circu.js"
    CACHE PATH "Path to the circu.js/libcjs source tree")
if(NOT EXISTS "${CIRCUD_LIBCJS_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "CIRCUD_LIBCJS_SOURCE does not point to circu.js: ${CIRCUD_LIBCJS_SOURCE}")
endif()

# Compile the host bridge into libcjs as one native ES module. It is
# intentionally absent from import.meta.use(); package code imports it with
# the standard `circud` module specifier.
set(CJS_EXTRA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/host_module.cpp"
    CACHE INTERNAL "" FORCE)
set(CJS_EXTRA_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include;${CMAKE_CURRENT_SOURCE_DIR}/src"
    CACHE INTERNAL "" FORCE)
set(CJS_EXTRA_LIBS "" CACHE INTERNAL "" FORCE)
set(CJS_EXTRA_DEFINES "" CACHE INTERNAL "" FORCE)
set(CJS_EXTRA_MODULE_NAMES "circud" CACHE INTERNAL "" FORCE)
set(CJS_EXTRA_MODULE_INITS "circud_module_init" CACHE INTERNAL "" FORCE)
set(CJS_EXTRA_MODULE_WORKER_SAFE "true" CACHE INTERNAL "" FORCE)
set(CJS_EXTRA_MODULE_USE "false" CACHE INTERNAL "" FORCE)
set(CJS_EXTRA_MODULE_ESM "true" CACHE INTERNAL "" FORCE)
set(BUILD_WITH_WASM OFF CACHE BOOL "" FORCE)
set(ENABLE_ASAN ${CIRCUD_ENABLE_ASAN} CACHE BOOL "" FORCE)
add_subdirectory("${CIRCUD_LIBCJS_SOURCE}" "${CMAKE_BINARY_DIR}/libcjs"
                 EXCLUDE_FROM_ALL)

add_library(circud_lib
    src/auth.cpp
    src/config.cpp
    src/core.cpp
    src/database.cpp
    src/endpoint_gateway.cpp
    src/endpoint_session.cpp
    src/event_journal.cpp
    src/http_auth.cpp
    src/http_background.cpp
    src/http_route.cpp
    src/http_server.cpp
    src/json.cpp
    src/manifest.cpp
    src/package_runtime.cpp
    src/state_lock.cpp
    src/types.cpp
    src/zip_installer.cpp
)
target_include_directories(circud_lib
    PUBLIC include
    PRIVATE src "${CIRCUD_LIBCJS_SOURCE}/src"
            "${CIRCUD_LIBCJS_SOURCE}/deps/quickjs"
            "${CIRCUD_LIBCJS_SOURCE}/deps/libuv/include")
target_compile_definitions(circud_lib PUBLIC
    BOOST_ERROR_CODE_HEADER_ONLY
    BOOST_SYSTEM_NO_DEPRECATED)
target_compile_definitions(circud_lib PRIVATE
    CIRCUD_VERSION="${PROJECT_VERSION}")
if(CIRCUD_ENABLE_STRICT_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(circud_lib PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
if(CIRCUD_ENABLE_ASAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(circud_lib PUBLIC -fsanitize=address
                                             -fno-omit-frame-pointer)
    target_link_options(circud_lib PUBLIC -fsanitize=address)
endif()
target_link_libraries(circud_lib
    PUBLIC libcjs Threads::Threads SQLite::SQLite3 OpenSSL::SSL OpenSSL::Crypto
           PkgConfig::MINIZIP ZLIB::ZLIB Boost::json)
if(UNIX AND NOT APPLE)
    # QuickJS and libuv use compiler atomics which require the GCC runtime on
    # some 32-bit OpenWrt targets (notably ARM and MIPS).
    target_link_libraries(circud_lib PUBLIC ${CMAKE_DL_LIBS} atomic)
endif()

add_executable(circud src/main.cpp)
target_include_directories(circud PRIVATE
    "${CIRCUD_LIBCJS_SOURCE}/deps/quickjs"
    "${CIRCUD_LIBCJS_SOURCE}/deps/libuv/include")
target_link_libraries(circud PRIVATE circud_lib)
target_compile_definitions(circud PRIVATE
    CIRCUD_VERSION="${PROJECT_VERSION}"
    CIRCUD_INSTALL_PUBLIC_DIR="${CMAKE_INSTALL_FULL_DATADIR}/circud/public"
    CIRCUD_SOURCE_PUBLIC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/public")
set_target_properties(circud PROPERTIES ENABLE_EXPORTS ON)
if(CIRCUD_ENABLE_STRICT_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(circud PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()

add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/demo.zip"
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${CMAKE_BINARY_DIR}/demo.zip"
            --format=zip -- meta.json management.js static/index.html
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/examples/demo"
    DEPENDS
        examples/demo/meta.json
        examples/demo/management.js
        examples/demo/static/index.html
    VERBATIM)
add_custom_target(circud_demo_package DEPENDS "${CMAKE_BINARY_DIR}/demo.zip")

if(BUILD_TESTING)
    add_executable(circud_tests
        tests/test_main.cpp
        tests/auth_test.cpp
        tests/config_test.cpp
        tests/database_test.cpp
        tests/endpoint_gateway_test.cpp
        tests/event_journal_test.cpp
        tests/http_auth_test.cpp
        tests/http_route_test.cpp
        tests/http_server_test.cpp
        tests/manifest_test.cpp
        tests/runtime_test.cpp
        tests/zip_installer_test.cpp)
    target_include_directories(circud_tests PRIVATE
        "${CIRCUD_LIBCJS_SOURCE}/deps/quickjs"
        "${CIRCUD_LIBCJS_SOURCE}/deps/libuv/include")
    target_link_libraries(circud_tests PRIVATE circud_lib)
    # libcjs is an excluded implementation dependency; keep its public native
    # ABI regressions in circud's normal build-and-test gate explicitly.
    add_dependencies(circud_tests capability_test evbus_api_test
                                  message_clone_test)
    if(CIRCUD_ENABLE_STRICT_WARNINGS AND
       CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(circud_tests PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME circud_tests COMMAND circud_tests)
    add_test(NAME daemon_bounded_shutdown
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon_bounded_shutdown_test.sh"
                   $<TARGET_FILE:circud>)
    set_tests_properties(daemon_bounded_shutdown PROPERTIES TIMEOUT 10)
endif()

install(TARGETS circud RUNTIME DESTINATION bin)
install(FILES packaging/circud.init DESTINATION share/circud/openwrt)
install(FILES config.example.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/circud
        RENAME circud.example.json)
install(DIRECTORY public/ DESTINATION ${CMAKE_INSTALL_DATADIR}/circud/public)
