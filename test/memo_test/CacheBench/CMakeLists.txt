# memo_test2：存储层性能测试（VS 中可单独设为启动项）
# 存储层源码由父目录 memo_test/CMakeLists.txt 收集并通过 STORAGE_SOURCES 传入
add_executable(memo_test2 test2.cpp ${STORAGE_SOURCES})

# memo_test3：写并发（在途窗口）版基准，与 memo_test2 同负载定义、可作 A/B 对照
add_executable(memo_test3 test3.cpp ${STORAGE_SOURCES})

# 头文件搜索路径：与主程序一致（代码统一在 project/ 下，include 时从层名开始写）
foreach(tgt memo_test2 memo_test3)
    target_include_directories(${tgt} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../project"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../project/third_party_lib/cereal_lib/include"
    )
endforeach()
