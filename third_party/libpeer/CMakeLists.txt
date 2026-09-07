cmake_minimum_required(VERSION 3.16)
cmake_policy(VERSION 3.16)

project(peer C)

option(ENABLE_TESTS "Enable tests" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(DISABLE_PEER_SIGNALING "Build without coreHTTP/coreMQTT signaling" ON)
option(BUILD_EXAMPLES "Build example programs" OFF)

if(NOT DISABLE_PEER_SIGNALING)
  include(${CMAKE_CURRENT_LIST_DIR}/third_party/coreHTTP/httpFilePaths.cmake)
  include(${CMAKE_CURRENT_LIST_DIR}/third_party/coreMQTT/mqttFilePaths.cmake)
  add_definitions("-DHTTP_DO_NOT_USE_CUSTOM_CONFIG -DMQTT_DO_NOT_USE_CUSTOM_CONFIG")
endif()

set(DEP_LIBS "srtp2" "usrsctp" "mbedtls" "mbedcrypto" "mbedx509")

add_definitions("-Wunused-variable -Werror=sequence-point -Werror=pointer-sign -Werror=return-type -Werror=sizeof-pointer-memaccess -Wincompatible-pointer-types")

add_subdirectory(src)

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(ENABLE_TESTS)
  add_subdirectory(tests)
endif()
