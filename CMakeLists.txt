cmake_minimum_required(VERSION 3.16)

# 项目信息
project(myDB VERSION 0.1.0 LANGUAGES CXX)

# ============ 全局配置 ============
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 未指定构建类型时默认 Release（对 VS 等单配置生成器无效，需在 IDE 中选）
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 编译警告
if(MSVC)
    # /utf-8：源文件与执行字符集按 UTF-8 解析（VS 默认按系统代码页 936/GBK，会乱码报 C4819）
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ============ 源文件 ============
# 递归收集 project/ 下各层（data_memo_level / command_map_level / net_level ...）的 .cpp
# CONFIGURE_DEPENDS 会在新增文件后自动重新配置
file(GLOB_RECURSE MYDB_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/project/*.cpp"
)

# ============ 生成可执行文件 ============
add_executable(myDB ${MYDB_SOURCES} "project/data_memo_level/cache/cache.cpp" "project/data_memo_level/data_type.h" "project/data_memo_level/disk/disk.cpp")

# 头文件搜索路径：代码统一在 project/ 下，include 时从层名开始写
# 例如 #include "data_memo_level/memory/memo.h"
target_include_directories(myDB PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/project"
)

# ============ 可选：链接第三方库 ============
# find_package(SomeLib REQUIRED)
# target_link_libraries(myDB PRIVATE SomeLib::SomeLib)

# ============ 可选：测试子目录 ============
# add_subdirectory(tests)
