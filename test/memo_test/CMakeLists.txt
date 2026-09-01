# 存储层测试：test/memo_test/ 下每个子目录是一个测试项目（各自含 CMakeLists.txt）
# 新增子测试只需新建目录 + CMakeLists.txt，此处自动收集，无需修改任何 CMakeLists

# 存储层源码：各子测试共享（add_subdirectory 的子目录 scope 继承本变量）
# 注意：main.cpp 在项目根目录，不在 project/ 下，不会被收集到测试工程中
file(GLOB_RECURSE STORAGE_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../project/*.cpp"
)
# 排除第三方库源码（cereal 自带 sandbox/unittests 依赖 boost，不参与构建）
list(FILTER STORAGE_SOURCES EXCLUDE REGEX "third_party_lib")

# 收集子测试目录
file(GLOB TEST_DIRS
    LIST_DIRECTORIES true
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*"
)
foreach(dir ${TEST_DIRS})
    if(EXISTS "${dir}/CMakeLists.txt")
        add_subdirectory(${dir})
    endif()
endforeach()
