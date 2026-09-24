# 存储层测试：test/memo_test/ 下每个子目录是一个测试项目（各自含 CMakeLists.txt）
# 新增子测试只需新建目录 + CMakeLists.txt，此处自动收集，无需修改任何 CMakeLists

# 存储层源码：各存储子测试共享；网络实现使用独立目标，避免被存储目标无条件链接。
file(GLOB_RECURSE STORAGE_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../project/data_memo_level/*.cpp"
)

# 收集子测试目录
file(GLOB TEST_DIRS
    LIST_DIRECTORIES true
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*"
)
foreach(dir ${TEST_DIRS})
    if(EXISTS "${dir}/CMakeLists.txt")
        if(dir STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/net_test")
            add_subdirectory(${dir} EXCLUDE_FROM_ALL)
        else()
            add_subdirectory(${dir})
        endif()
    endif()
endforeach()
