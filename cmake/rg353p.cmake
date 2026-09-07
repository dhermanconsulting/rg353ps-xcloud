# CMake toolchain file for the Anbernic RG353 (aarch64, glibc 2.32 ceiling).
# Use inside the arm64 bullseye container, where the compiler is native aarch64:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/rg353p.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(RG353_C_FLAGS "-march=armv8-a -mtune=cortex-a55 -O2")
set(CMAKE_C_FLAGS_INIT   "${RG353_C_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${RG353_C_FLAGS}")

# Never fully static: getaddrinfo dlopen()s libnss_* at runtime and a -static
# build silently loses DNS. Static only the C++ runtime and our vendored deps.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libstdc++ -static-libgcc")
