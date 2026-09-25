/*
    模块名：网络服务端
    功能描述：编排监听器、ReactorGroup 与命令执行器的生命周期。
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
    参数：config：服务端配置；executor：命令执行器
    功能：创建服务端并保存运行依赖
    返回：无
*/
NetworkServer::NetworkServer(ServerConfig config, std::shared_ptr<ICommandExecutor> executor)
    : config(std::move(config)), executor(std::move(executor)) {}

NetworkServer::~NetworkServer() {
    stop();
}

/*
    函数：start
    参数：无
    功能：启动 ReactorGroup 和 TCP 监听器
    返回：是否启动成功
*/
bool NetworkServer::start() {
    if (state == ServerState::RUNNING) {
        return true;
    }
    // Require a numeric loopback literal. Hostnames are deliberately not trusted
    // here because name-service resolution could bind to a non-loopback address.
    const bool loopback = config.listenEndpoint.address == "127.0.0.1" ||
                          config.listenEndpoint.address == "::1" ||
                          config.listenEndpoint.address == "::ffff:127.0.0.1";
    if (state != ServerState::CREATED || !executor || config.reactorCount > UINT32_MAX || config.maxClients == 0 ||
        config.maxPipelineRequests == 0 || config.maxFrameBytes < BASE_HEADER_LENGTH ||
        config.maxFrameBytes > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        (config.tlsRequired && (config.tlsCertificateFile.empty() || config.tlsPrivateKeyFile.empty())) ||
        (!config.tlsRequired && !config.allowPlaintextForDevelopment) ||
        (!config.tlsRequired && !loopback && !config.allowInsecureRemote)) {
        return false;
    }

    tlsContext.reset();
    if (config.tlsRequired) {
        tlsContext = TlsServerContext::create(config.tlsCertificateFile, config.tlsPrivateKeyFile);
        if (!tlsContext) {
            return false;
        }
    }

    std::size_t reactorCount = config.reactorCount;
    if (reactorCount == 0) {
        reactorCount = std::max<std::size_t>(1, std::thread::hardware_concurrency() / 2);
        reactorCount = std::min<std::size_t>(reactorCount, 16);
    }
    reactorGroup = std::make_unique<ReactorGroup>(reactorCount, executor);
    if (!reactorGroup->start()) {
        reactorGroup.reset();
        return false;
    }

    acceptor = std::make_unique<Acceptor>(config.listenEndpoint, 1024,
        [this](std::unique_ptr<Transport> transport, const Endpoint& peer, ClientSlotRelease releaseSlot) {
            if (config.tlsRequired) {
                transport = std::make_unique<TlsTransport>(std::move(transport), tlsContext);
            }
            const ConnectionId connectionId = nextConnectionId.fetch_add(1, std::memory_order_relaxed);
            if (!reactorGroup->dispatch(std::move(transport), peer, config.maxPipelineRequests,
                                        connectionId, releaseSlot, config.maxFrameBytes)) {
                releaseSlot();
            }
        });
    acceptor->maxClients = config.maxClients;
    acceptor->activeClients = &activeClients;
    if (!acceptor->start()) {
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
    参数：无
    功能：停止 listener、立即关闭连接并停止 ReactorGroup
    返回：无
*/
void NetworkServer::stop() {
    if (state == ServerState::STOPPED) {
        return;
    }
    if (state == ServerState::CREATED) {
        state = ServerState::STOPPED;
        return;
    }
    state = ServerState::STOPPED;
    if (acceptor) {
        acceptor->stop();
        acceptor.reset();
    }
    if (reactorGroup) {
        reactorGroup->stop();
        reactorGroup.reset();
    }
}

/*
    函数：wait
    参数：无
    功能：等待服务线程退出；当前 stop() 同步 join 所有服务线程
    返回：无
*/
void NetworkServer::wait() {}

/*
    函数：getState
    参数：无
    功能：读取网络服务当前状态
    返回：ServerState
*/
ServerState NetworkServer::getState() const {
    return state;
}

}
