add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../project/net_level
                 ${CMAKE_CURRENT_BINARY_DIR}/net_level)

add_executable(net_test net_test.cpp)

target_include_directories(net_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../project"
)

target_link_libraries(net_test PRIVATE mydb_net)

add_executable(net_correctness_test net_correctness_test.cpp)
target_include_directories(net_correctness_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../project"
)
target_link_libraries(net_correctness_test PRIVATE mydb_net_linux)

add_executable(net_stress_test net_stress_test.cpp)
target_include_directories(net_stress_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../project"
)
target_link_libraries(net_stress_test PRIVATE mydb_net_linux)
