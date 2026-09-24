/*
    模块名：Reactor 组
    功能描述：创建和管理 Linux epoll Reactor，并将每个客户端固定分派到一个事件循环。
*/

#include "reactor_group.h"

#include "epoll_poller.h"

#include <limits>
#include <utility>

namespace mydb::net {

/*
    函数：ReactorGroup
    参数：reactorCount：Reactor 数量；executor：共享命令执行器
    功能：创建 Reactor 及其 epoll poller
    返回：无
*/
ReactorGroup::ReactorGroup(std::size_t reactorCount, std::shared_ptr<ICommandExecutor> executor) {
    if (reactorCount > std::numeric_limits<ReactorId>::max()) {
        return;
    }
    reactors.reserve(reactorCount);
    for (std::size_t i = 0; i < reactorCount; ++i) {
        reactors.push_back(std::make_unique<Reactor>(static_cast<ReactorId>(i),
            std::make_unique<EpollPoller>(), executor));
    }
}

/*
    函数：start
    参数：无
    功能：启动所有 Reactor 线程
    返回：是否全部启动成功
*/
bool ReactorGroup::start() {
    std::lock_guard<std::mutex> lock(mutex);
    if (running) {
        return true;
    }
    if (reactors.empty()) {
        return false;
    }
    std::size_t started = 0;
    try {
        for (auto& reactor : reactors) {
            reactor->start();
            ++started;
        }
    } catch (...) {
        for (std::size_t index = 0; index < started; ++index) {
            reactors[index]->stop(false);
        }
        return false;
    }
    running = true;
    return true;
}

/*
    函数：stop
    参数：drain：是否优雅排空已提交请求
    功能：停止所有 Reactor 并等待事件线程退出
    返回：无
*/
void ReactorGroup::stop(bool drain) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!running) {
        return;
    }
    running = false;
    for (auto& reactor : reactors) {
        reactor->stop(drain);
    }
}

/*
    函数：dispatch
    参数：transport：已接受的非阻塞传输；peer：对端地址；maxPipelineRequests：流水线上限
    功能：将连接绑定到负载轮询选出的 Reactor
    返回：是否成功分派
*/
bool ReactorGroup::dispatch(std::unique_ptr<Transport> transport, Endpoint peer,
                            std::size_t maxPipelineRequests, ConnectionId connectionId,
                            ClientSlotRelease releaseSlot, std::uint32_t maxFrameBytes) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!running || reactors.empty()) {
        return false;
    }
    auto& reactor = reactors[nextReactor];
    nextReactor = (nextReactor + 1) % reactors.size();
    return reactor->addConnection(connectionId, std::move(transport), std::move(peer), maxPipelineRequests,
                                  std::move(releaseSlot), maxFrameBytes);
}

}
