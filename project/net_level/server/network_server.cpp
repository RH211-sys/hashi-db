/*
    模块名：网络服务端
    模块地位：网络服务控制器背后的监听与事件循环生命周期编排。
    模块功能描述：编排监听器、ReactorGroup 与命令执行器的生命周期。
*/

#include "network_server.h"

#include "../transport/tcp_transport.h"
#include "../transport/tls_transport.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>

namespace mydb::net {


/*
    函数：NetworkServer
    传参：config：服务端配置；executor：命令执行器
    功能：创建服务端并保存运行依赖
    返回值：无
*/
NetworkServer::NetworkServer(ServerConfig config, std::shared_ptr<ICommandExecutor> executor)
    : config(std::move(config)), executor(std::move(executor)) {}

/*
    功能：销毁服务端前停止监听器和所有 Reactor
    传参：无
    返回值：无
*/
NetworkServer::~NetworkServer() {
    stop();
}

/*
    函数：start
    传参：无
    功能：启动 ReactorGroup 和 TCP 监听器
    返回值：是否启动成功
*/
bool NetworkServer::start() {
    if (state == ServerState::RUNNING) {
        // 服务端已经运行，重复启动视为成功。
        return true;
    }
    // 仅接受数字形式的回环地址，不信任可能被解析为非回环地址的主机名。
    const bool loopback = config.listenEndpoint.address == "127.0.0.1" ||
                          config.listenEndpoint.address == "::1" ||
                          config.listenEndpoint.address == "::ffff:127.0.0.1"; // 回环判断：限制明文连接仅用于本机地址
    if (state != ServerState::CREATED || !executor || config.reactorCount > UINT32_MAX || config.maxClients == 0 ||
        config.maxPipelineRequests == 0 || config.maxFrameBytes < BASE_HEADER_LENGTH ||
        config.maxFrameBytes > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        (config.tlsRequired && (config.tlsCertificateFile.empty() || config.tlsPrivateKeyFile.empty())) ||
        (!config.tlsRequired && !config.allowPlaintextForDevelopment) ||
        (!config.tlsRequired && !loopback && !config.allowInsecureRemote)) {
        // 服务状态、依赖、容量、帧上限或明文传输安全配置无效，拒绝启动。
        return false;
    }

    tlsContext.reset();
    if (config.tlsRequired) {
        // 配置要求 TLS，创建并保存服务端 TLS 上下文。
        tlsContext = TlsServerContext::create(config.tlsCertificateFile, config.tlsPrivateKeyFile);
        if (!tlsContext) {
            // TLS 上下文创建失败，拒绝启动服务。
            return false;
        }
    }

    std::size_t reactorCount = config.reactorCount; // Reactor 数量：0 时按机器逻辑核数推导默认值
    if (reactorCount == 0) {
        // 未指定 Reactor 数量，根据逻辑核数计算默认值并限制最大数量。
        reactorCount = std::max<std::size_t>(1, std::thread::hardware_concurrency() / 2);
        reactorCount = std::min<std::size_t>(reactorCount, 16);
    }
    reactorGroup = std::make_unique<ReactorGroup>(reactorCount, executor);
    if (!reactorGroup->start()) {
        // Reactor 组启动失败，释放组对象并拒绝启动服务。
        reactorGroup.reset();
        return false;
    }

    acceptor = std::make_unique<Acceptor>(config.listenEndpoint, 1024,
        [this](std::unique_ptr<Transport> transport, const Endpoint& peer, ClientSlotRelease releaseSlot) { // transport、peer、releaseSlot：新连接及其地址和槽位释放回调
            if (config.tlsRequired) {
                // 服务端启用 TLS，将新 TCP 传输封装为 TLS 传输。
                transport = std::make_unique<TlsTransport>(std::move(transport), tlsContext);
            }
            const ConnectionId connectionId = nextConnectionId.fetch_add(1, std::memory_order_relaxed); // 连接标识：为新接入分配全局唯一 ID
            if (!reactorGroup->dispatch(std::move(transport), peer, config.maxPipelineRequests,
                                        connectionId, releaseSlot, config.maxFrameBytes)) {
                // Reactor 组拒绝接入连接，归还预留的客户端槽位。
                releaseSlot();
            }
        });
    acceptor->maxClients = config.maxClients;
    acceptor->activeClients = &activeClients;
    if (!acceptor->start()) {
        // 监听器启动失败，关闭已创建的监听和 Reactor 资源。
        acceptor.reset();
        reactorGroup->stop();
        reactorGroup.reset();
        return false;
    }
    state = ServerState::RUNNING;
    return true;
}

/*
    函数：stop
    传参：无
    功能：停止 listener、立即关闭连接并停止 ReactorGroup
    返回值：无
*/
void NetworkServer::stop() {
    if (state == ServerState::STOPPED) {
        // 服务端已经停止，避免重复清理。
        return;
    }
    if (state == ServerState::CREATED) {
        // 服务端尚未启动，直接记录停止状态。
        state = ServerState::STOPPED;
        return;
    }
    state = ServerState::STOPPED;
    if (acceptor) {
        // 监听器已创建，先停止接收新连接并释放监听资源。
        acceptor->stop();
        acceptor.reset();
    }
    if (reactorGroup) {
        // Reactor 组已创建，停止所有事件线程并关闭管理资源。
        reactorGroup->stop();
        reactorGroup.reset();
    }
}

/*
    函数：wait
    传参：无
    功能：等待服务线程退出；当前 stop() 同步 join 所有服务线程
    返回值：无
*/
void NetworkServer::wait() {}

/*
    函数：getState
    传参：无
    功能：读取网络服务当前状态
    返回值：ServerState
*/
ServerState NetworkServer::getState() const {
    return state;
}

}
