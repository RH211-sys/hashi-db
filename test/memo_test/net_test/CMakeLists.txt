add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../project/net_level
                 ${CMAKE_CURRENT_BINARY_DIR}/net_level)

add_executable(net_test net_test.cpp)

target_include_directories(net_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../project"
)

target_link_libraries(net_test PRIVATE mydb_net)
