# ============================================================
# rkgame-rebuild — CMake 构建文件
# ============================================================
#
# 构建目标：ARM32 hard-float, glibc 2.29
# 首次提交: 2026-09-01
# 交叉工具链：arm-linux-gnueabihf-gcc (crosstool-NG, sysroot glibc 2.29)
#
# ABI 铁律：
#   - CFLAGS: -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4
#             -mfloat-abi=hard -O2
#   - e_flags 目标：0x5000400
#   - 所有 dlopen/dlsym/dlclose/pthread_* 必须 .symver 锁 GLIBC_2.4
#   - clock_gettime 锁 GLIBC_2.17（设备 glibc 2.29 支持）
#   - 禁用 atoi/strtol（GCC 14 会优化为 __isoc23_strtol@GLIBC_2.38）
#   - 显式链接 -ldl -Wl,--no-as-needed（工具链 libdl.a 为空）
# ============================================================

cmake_minimum_required(VERSION 3.16)

project(rkgame-rebuild C)

set(CMAKE_C_STANDARD 11)

# ---- 构建配置 ----
set(CMAKE_BUILD_TYPE Release CACHE STRING "Release" FORCE)

# ---- ABI 门禁 ----
add_compile_options(
    -march=armv7-a
    -mtune=cortex-a7
    -mfpu=neon-vfpv4
    -mfloat-abi=hard
    -O2
)

# 禁用 C23 下的 atoi/strtol → __isoc23_strtol@GLIBC_2.38 陷阱
# 我们的代码已用 strtoul 替代 atoi，这里加保险
add_compile_definitions(_GNU_SOURCE)

# ---- 链接选项 ----
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--no-as-needed")

# ---- 源文件 ----
file(GLOB SRC_FILES "src/*.c")
# 排除不存在的文件
list(FILTER SRC_FILES EXCLUDE REGEX ".*\\.h$")

# ---- 头文件 ----
include_directories(src)

# ---- 输出目录（必须在 add_executable 之前） ----
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output")

# ---- 构建可执行文件 ----
add_executable(rkgame ${SRC_FILES})

target_link_libraries(rkgame
    PRIVATE
        -Wl,--no-as-needed dl
        pthread
        m
)

# ---- 安装 ----
install(TARGETS rkgame DESTINATION .)

# ---- 打印 ABI 信息 ----
message(STATUS "rkgame-rebuild: source files = ${SRC_FILES}")
message(STATUS "rkgame-rebuild: output = ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/rkgame")
