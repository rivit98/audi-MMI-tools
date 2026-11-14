# libaa_jni.so: the Android Auto head unit session of src/android_auto
# (session, frames, TLS over mbedTLS), the same C++ as the car app's, for
# the phone app's Android Auto mode (AaService). The JNI glue is aa_jni.cpp.
# mbedTLS and our config come from the repo (third_party/, fetched by the
# top Makefile: make -C .. aa-sources), net_sockets.c left out as on the car.
cmake_minimum_required(VERSION 3.22.1)
project(aa_jni C CXX)

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)
set(MBEDTLS ${ROOT}/third_party/mbedtls-3.6.7)

file(GLOB MBEDTLS_SRC ${MBEDTLS}/library/*.c)
list(FILTER MBEDTLS_SRC EXCLUDE REGEX ".*/net_sockets\\.c$")
add_library(mbedtls_aa STATIC ${MBEDTLS_SRC})
target_include_directories(mbedtls_aa PUBLIC ${ROOT}/src/common/mbedtls ${MBEDTLS}/include)
target_compile_definitions(mbedtls_aa PUBLIC "MBEDTLS_CONFIG_FILE=\"mbedtls_config.h\"")
target_compile_options(mbedtls_aa PRIVATE -O2)

add_library(aa_jni SHARED
    aa_jni.cpp
    ${ROOT}/src/android_auto/session.cpp
    ${ROOT}/src/android_auto/tls.cpp
    ${ROOT}/src/common/log.c)
set_target_properties(aa_jni PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(aa_jni PRIVATE ${ROOT}/src/android_auto ${ROOT}/src/common)
target_compile_options(aa_jni PRIVATE -Wall -Wextra -O2)
target_link_libraries(aa_jni mbedtls_aa log)
