# memo_test1：存储层测试项目 1（VS 中可单独设为启动项）
# 存储层源码由父目录 memo_test/CMakeLists.txt 收集并通过 STORAGE_SOURCES 传入
add_executable(memo_test1 test1.cpp ${STORAGE_SOURCES})

# memo_probe1：add/del/sel 返回码最小探针（诊断用）
add_executable(memo_probe1 probe1.cpp ${STORAGE_SOURCES})

# 头文件搜索路径：与主程序一致（代码统一在 project/ 下，include 时从层名开始写）
foreach(tgt memo_test1 memo_probe1)
    target_include_directories(${tgt} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../project"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../project/third_party_lib/cereal_lib/include"
    )
endforeach()
