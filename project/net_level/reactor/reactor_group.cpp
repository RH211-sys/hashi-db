/*
    模块名：Reactor 组
    模块地位：创建并管理 Linux Reactor，按序分派新连接。
    模块功能描述：创建和管理 Linux epoll Reactor，并将每个客户端固定分派到一个事件循环。
*/

#include "reactor_group.h"

#include "epoll_poller.h"

#include <limits>
#include <utility>

namespace mydb::net {

/*
    函数：ReactorGroup
    传参：reactorCount：Reactor 数量；executor：共享命令执行器
    功能：创建 Reactor 及其 epoll poller
    返回值：无
*/
ReactorGroup::ReactorGroup(std::size_t reactorCount, std::shared_ptr<ICommandExecutor> executor) {
    if (reactorCount > std::numeric_limits<ReactorId>::max()) {
        // Reactor 数量超出标识类型可表示范围，保留空组并结束构造。
        return;
    }
    reactors.reserve(reactorCount);
    for (std::size_t i = 0; i < reactorCount; ++i) { // Reactor 下标：按编号创建事件循环实例
        reactors.push_back(std::make_unique<Reactor>(static_cast<ReactorId>(i),
            std::make_unique<EpollPoller>(), executor));
    }
}

/*
    函数：start
    传参：无
    功能：启动所有 Reactor 线程
    返回值：是否全部启动成功
*/
bool ReactorGroup::start() {
    std::lock_guard<std::mutex> lock(mutex);        // 分配锁：保护 Reactor 启停和轮询分配位置
    if (running) {
        // Reactor 组已经运行，避免重复启动并报告成功。
        return true;
    }
    if (reactors.empty()) {
        // 没有可启动的 Reactor，报告启动失败。
        return false;
    }
    std::size_t started = 0;                       // 已启动数量：异常回滚时只停止成功启动的 Reactor
    try {
        for (auto& reactor : reactors) {            // 当前 Reactor：依次启动事件循环
            reactor->start();
            ++started;
        }
    } catch (...) {
        // 部分 Reactor 启动失败，停止已成功启动的实例并回滚组状态。
        for (std::size_t index = 0; index < started; ++index) { // Reactor 下标：回滚已成功启动的事件循环
            reactors[index]->stop();
        }
        return false;
    }
    running = true;
    return true;
}

/*
    函数：stop
    传参：无
    功能：立即停止所有 Reactor 并等待事件线程退出
    返回值：无
*/
void ReactorGroup::stop() {
    std::lock_guard<std::mutex> lock(mutex);        // 分配锁：保护 Reactor 启停和轮询分配位置
    if (!running) {
        // Reactor 组尚未运行，无需停止其事件线程。
        return;
    }
    running = false;
    for (auto& reactor : reactors) {                // 当前 Reactor：依次停止并回收事件线程
        reactor->stop();
    }
}

/*
    函数：dispatch
    传参：transport：已接受的非阻塞传输；peer：对端地址；maxPipelineRequests：流水线上限；connectionId：连接标识
          releaseSlot：连接槽位释放回调；maxFrameBytes：单帧字节上限
    功能：将连接绑定到负载轮询选出的 Reactor
    返回值：是否成功分派
*/
bool ReactorGroup::dispatch(std::unique_ptr<Transport> transport, Endpoint peer,
                            std::size_t maxPipelineRequests, ConnectionId connectionId,
                            ClientSlotRelease releaseSlot, std::uint32_t maxFrameBytes) {
    std::lock_guard<std::mutex> lock(mutex);        // 分配锁：保护 Reactor 启停和轮询分配位置
    if (!running || reactors.empty()) {
        // Reactor 组未运行或没有可用实例，拒绝分派连接。
        return false;
    }
    auto& reactor = reactors[nextReactor];          // 目标 Reactor：按轮询位置分配新连接
    nextReactor = (nextReactor + 1) % reactors.size();
    return reactor->addConnection(connectionId, std::move(transport), std::move(peer), maxPipelineRequests,
                                  std::move(releaseSlot), maxFrameBytes);
}

}
