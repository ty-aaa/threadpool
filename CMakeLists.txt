cmake_minimum_required(VERSION 3.16)
project(Threadpool CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Sanitizer ────────────────────────────────────────────────────
option(ENABLE_ASAN "启用 AddressSanitizer + UBSan" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# W5 的主战场：数据竞争。ASan 看不见它，要用 TSan。
# ⚠️ ASan 和 TSan 不能同时开 —— 用两个独立的构建目录。
#   cmake -B build-tsan -G Ninja -DENABLE_TSAN=ON
option(ENABLE_TSAN "启用 ThreadSanitizer（数据竞争检测）" OFF)
if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
endif()

# ── 编译警告设置 ──────────────────────────────────────────────────
add_library(project_warnings INTERFACE)
if(MSVC)
    target_compile_options(project_warnings INTERFACE /W4)
else()
    target_compile_options(project_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# ── 第三方依赖 ───────────────────────────────────────────────────
find_package(GTest REQUIRED)

# ── 核心库 ───────────────────────────────────────────────────────
# 注意：如果你有 .cpp 实现文件，必须用 STATIC。如果全在头文件，用 INTERFACE。
# 假设你有 src/vector.cpp
add_library(mythreadpool INTERFACE) 
target_include_directories(mythreadpool INTERFACE include)
target_link_libraries(mythreadpool INTERFACE project_warnings) # 传递给所有使用者

# ── 示例程序 ─────────────────────────────────────────────────────
add_executable(myapp examples/main.cpp)
# 主程序不需要 GTest，只链接 mythreadpool 即可
target_link_libraries(myapp PRIVATE mythreadpool) 

# ── 单元测试 ─────────────────────────────────────────────────────
enable_testing()
add_executable(test_threadpool tests/test_threadpool.cpp)
# 测试程序链接 mythreadpool 和 GTest
target_link_libraries(test_threadpool PRIVATE mythreadpool GTest::gtest_main)

add_test(NAME test_threadpool COMMAND test_threadpool)

# 死锁检测：线程池一旦卡死，测试会永远挂着。
# 给个超时，让 ctest 在 5 分钟后判失败，而不是让 CI 悬停。
set_tests_properties(test_threadpool PROPERTIES TIMEOUT 300)

# 线程池本身需要 pthread（即使不链接 GTest 也要）
find_package(Threads REQUIRED)
target_link_libraries(mythreadpool INTERFACE Threads::Threads)