# 网络构建目标分层；全部 EXCLUDE_FROM_ALL，不进入存储测试默认构建。
#
# mydb_net_core             平台无关的协议与连接核心
# mydb_net_linux            Linux 网络服务端装配（不含存储依赖）
# mydb_net_storage_bridge   阶段测试用存储桥接（需要存储层与 cereal 头文件）
# mydb_net                  聚合接口目标，供需要完整服务端的测试使用

add_library(mydb_net_core STATIC EXCLUDE_FROM_ALL
    protocol/frame_codec.cpp
    command/command_parser.cpp
    server/session.cpp
    server/connection.cpp
    reactor/reactor.cpp
)

target_include_directories(mydb_net_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

add_library(mydb_net_linux STATIC EXCLUDE_FROM_ALL
    transport/tcp_transport.cpp
    transport/tls_transport.cpp
    reactor/epoll_poller.cpp
    reactor/reactor_group.cpp
    server/acceptor.cpp
    server/network_server.cpp
)

target_include_directories(mydb_net_linux PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(mydb_net_linux PUBLIC mydb_net_core)

find_package(OpenSSL 1.1.1 REQUIRED)
target_link_libraries(mydb_net_linux PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# 存储桥接是本层临时适配器：它引用存储层 Controller 与 cereal 序列化头文件，
# 因此单独成目标，避免纯网络目标被拖上存储层依赖。
add_library(mydb_net_storage_bridge STATIC EXCLUDE_FROM_ALL
    command/command_executor.cpp
    controller/controller.cpp
)

target_include_directories(mydb_net_storage_bridge PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_include_directories(mydb_net_storage_bridge PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party_lib/cereal_lib/include"
)

target_link_libraries(mydb_net_storage_bridge PUBLIC mydb_net_linux)

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(mydb_net_core PUBLIC Threads::Threads)
    target_link_libraries(mydb_net_linux PUBLIC Threads::Threads)
    target_link_libraries(mydb_net_storage_bridge PUBLIC Threads::Threads)
endif()

add_library(mydb_net INTERFACE)
target_link_libraries(mydb_net INTERFACE mydb_net_linux mydb_net_storage_bridge)
