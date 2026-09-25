/*
    模块名：Reactor 事件循环
    模块地位：网络层 I/O 调度核心，位于 ReactorGroup 与 Connection 之间
    模块功能描述：在单线程内驱动 Poller 和所属连接，并通过有界 inbox 接收跨线程任务与业务完成结果。
*/

#include "reactor.h"
#include "../server/connection.h"
#include "../protocol/protocol.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mydb::net {
namespace {

/*
    类型名：InboxKind
    功能：区分 Reactor 收件队列的用途，不同用途使用独立的容量上限。
*/
enum class InboxKind {
    POST,          // 通用任务：post() 提交的任意任务
    CONTROL,       // 控制任务：连接接入、优雅关闭和连接释放
    COMPLETION     // 完成结果：业务执行完毕后回投到连接所属 Reactor
};

constexpr std::size_t MAX_POST_TASKS = 4096;       // 通用任务上限：超出后丢弃后续任务
constexpr std::size_t MAX_CONTROL_TASKS = 256;     // 控制任务上限：超出后丢弃后续任务
constexpr std::size_t MAX_COMPLETION_TASKS = 4096; // 完成结果上限：超出后放弃该响应并关闭对应连接
constexpr std::size_t POLL_EVENT_BATCH = 128;      // 单次轮询最多处理的事件数
constexpr int POLL_WAIT_MS = 100;                  // 单次轮询等待毫秒数：也是停止检查的周期
constexpr std::size_t READ_BUDGET_BYTES = 64U * 1024U;  // 单次可读事件最多读取字节数
constexpr std::size_t WRITE_BUDGET_BYTES = 256U * 1024U; // 单次可写事件最多写出字节数

/*
    类型名：SlotRelease
    功能：保证连接槽位释放回调在所有持有者之间只执行一次。
        连接接入任务可能因队列满被丢弃，此时也必须释放已占用的客户端槽位。
*/
struct SlotRelease {
    std::atomic<bool> released{false};             // 释放标记：保证回调只执行一次
    ClientSlotRelease callback;                    // 槽位释放回调：由 Acceptor 提供

    explicit SlotRelease(ClientSlotRelease callback) : callback(std::move(callback)) {}

    /*
        函数：run
        参数：无
        功能：执行一次槽位释放回调
        返回：无
    */
    void run() {
        if (!released.exchange(true, std::memory_order_acq_rel) && callback) {
            callback();
        }
    }

    ~SlotRelease() {
        run();
    }
};

}

struct Reactor::Impl {
    ReactorId id;                                                     // Reactor 标识：用于回投与诊断
    std::unique_ptr<Poller> poller;                                   // 事件轮询器：驱动所属连接的 I/O
    std::shared_ptr<ICommandExecutor> executor;                       // 命令执行器：所有连接共享
    std::unordered_map<ConnectionId, std::unique_ptr<Connection>> connections; // 所属连接：仅本线程访问

    std::mutex inboxMutex;                                            // 保护以下全部队列和停止标记
    std::deque<ReactorTask> inbox;                                    // 通用任务队列
    std::deque<ReactorTask> controlInbox;                             // 控制任务队列
    std::deque<ReactorTask> completionInbox;                          // 完成结果队列
    std::unordered_set<ConnectionId> overflowConnections;             // 待关闭连接：完成结果被丢弃的连接

    std::thread worker;                                               // 事件线程：唯一执行 run() 的线程
    std::atomic<bool> running{false};                                 // 运行标记：false 时拒绝一切入队
    std::atomic<bool> stopping{false};                                // 停止标记：true 后不再接收新任务
    bool acceptingPosts = false;                                      // 接收入队标记：仅锁内访问
    std::uint64_t nextGeneration = 1;                                 // 连接代际：区分复用连接 ID 的旧完成结果

    Impl(ReactorId reactorId, std::unique_ptr<Poller> reactorPoller,
         std::shared_ptr<ICommandExecutor> commandExecutor)
        : id(reactorId), poller(std::move(reactorPoller)), executor(std::move(commandExecutor)) {}

    /*
        函数：enqueue
        参数：task：待执行任务；kind：队列用途；connectionId：完成结果所属连接
        功能：按用途把任务放入对应有界队列，队列已满时按既定规则丢弃
        返回：任务是否成功入队
        备注：完成结果队列满时放弃该响应，并请求关闭对应连接，避免连接停留在等待完成的状态
    */
    bool enqueue(ReactorTask task, InboxKind kind, ConnectionId connectionId = 0) {
        bool accepted = false;
        bool needWakeup = false;
        {
            std::lock_guard<std::mutex> lock(inboxMutex);
            if (!running.load(std::memory_order_acquire)) {
                return false;
            }
            const bool stoppingNow = stopping.load(std::memory_order_acquire);
            if (kind == InboxKind::COMPLETION) {
                if (stoppingNow) {
                    return false;
                }
                if (completionInbox.size() < MAX_COMPLETION_TASKS) {
                    completionInbox.push_back(std::move(task));
                    accepted = true;
                    needWakeup = true;
                } else if (connectionId != 0) {
                    overflowConnections.insert(connectionId);
                    needWakeup = true;
                }
            } else if (!acceptingPosts || stoppingNow) {
                return false;
            } else if (kind == InboxKind::CONTROL) {
                if (controlInbox.size() < MAX_CONTROL_TASKS) {
                    controlInbox.push_back(std::move(task));
                    accepted = true;
                    needWakeup = true;
                }
            } else if (inbox.size() < MAX_POST_TASKS) {
                inbox.push_back(std::move(task));
                accepted = true;
                needWakeup = true;
            }
        }
        if (needWakeup) {
            poller->wakeup();
        }
        return accepted;
    }

    /*
        函数：processInbox
        参数：无
        功能：取出当前全部待执行任务并在本线程依次执行，单个任务异常不影响事件循环
        返回：无
    */
    void processInbox() {
        std::deque<ReactorTask> tasks;
        {
            std::lock_guard<std::mutex> lock(inboxMutex);
            tasks.swap(controlInbox);
            while (!inbox.empty()) {
                tasks.push_back(std::move(inbox.front()));
                inbox.pop_front();
            }
            while (!completionInbox.empty()) {
                tasks.push_back(std::move(completionInbox.front()));
                completionInbox.pop_front();
            }
        }
        for (auto& task : tasks) {
            if (stopping.load(std::memory_order_acquire)) {
                break;
            }
            try {
                task();
            } catch (...) {
                // 单个任务失败不得中断事件循环。
            }
        }
    }

    /*
        函数：applyPendingClosures
        参数：无
        功能：关闭因完成结果被丢弃而标记的连接
        返回：无
    */
    void applyPendingClosures() {
        std::vector<ConnectionId> pending;
        {
            std::lock_guard<std::mutex> lock(inboxMutex);
            if (overflowConnections.empty()) {
                return;
            }
            pending.assign(overflowConnections.begin(), overflowConnections.end());
            overflowConnections.clear();
        }
        for (ConnectionId connectionId : pending) {
            closeConnection(connectionId);
        }
    }

    /*
        函数：discardQueuedTasks
        参数：无
        功能：丢弃全部未执行任务，并重置入队状态
        返回：无
        备注：接入任务持有连接槽位，丢弃时由捕获对象释放；本线程退出前调用
    */
    void discardQueuedTasks() {
        std::deque<ReactorTask> discarded;
        {
            std::lock_guard<std::mutex> lock(inboxMutex);
            acceptingPosts = false;
            overflowConnections.clear();
            discarded.swap(controlInbox);
            while (!inbox.empty()) {
                discarded.push_back(std::move(inbox.front()));
                inbox.pop_front();
            }
            while (!completionInbox.empty()) {
                discarded.push_back(std::move(completionInbox.front()));
                completionInbox.pop_front();
            }
        }
        discarded.clear();
    }

    /*
        函数：refreshInterest
        参数：connectionId：目标连接
        功能：按连接当前状态刷新 Poller 关注事件，连接已关闭或刷新失败时关闭连接
        返回：无
    */
    void refreshInterest(ConnectionId connectionId) {
        const auto found = connections.find(connectionId);
        if (found == connections.end()) {
            return;
        }
        if (found->second->getState() == ConnectionState::CLOSED) {
            closeConnection(connectionId);
            return;
        }
        const std::uint32_t events = found->second->interestEvents();
        if (found->second->getState() == ConnectionState::CLOSING &&
            found->second->peerClosed && events == 0) {
            if (found->second->pollerRegistered) {
                poller->remove(connectionId);
                found->second->pollerRegistered = false;
            }
            return;
        }
        if (events == 0) {
            if (found->second->pollerRegistered) {
                poller->remove(connectionId);
                found->second->pollerRegistered = false;
            }
            return;
        }
        const bool updated = found->second->pollerRegistered
            ? poller->modify(connectionId, events)
            : poller->add(connectionId, found->second->transport->nativeHandle(), events);
        if (!updated) {
            closeConnection(connectionId);
            return;
        }
        found->second->pollerRegistered = true;
    }

    /*
        函数：closeConnection
        参数：connectionId：目标连接
        功能：从 Poller 和连接表移除连接，释放 socket 与客户端槽位
        返回：无
        备注：只能在事件线程调用，且调用期间不得持有 inboxMutex
    */
    void closeConnection(ConnectionId connectionId) {
        const auto found = connections.find(connectionId);
        if (found == connections.end()) {
            return;
        }
        if (found->second->pollerRegistered) {
            poller->remove(connectionId);
            found->second->pollerRegistered = false;
        }
        found->second->close();
        connections.erase(found);
        std::lock_guard<std::mutex> lock(inboxMutex);
        overflowConnections.erase(connectionId);
    }

    /*
        函数：closeAllConnections
        参数：无
        功能：关闭当前全部连接
        返回：无
    */
    void closeAllConnections() {
        while (!connections.empty()) {
            closeConnection(connections.begin()->first);
        }
    }

    /*
        函数：handleEvents
        参数：events：本次轮询结果；count：有效事件数量
        功能：按事件驱动对应连接的读写、握手与关闭
        返回：无
    */
    void handleEvents(const PollEventItem* events, int count) {
        for (int index = 0; index < count; ++index) {
            const PollEventItem item = events[index];
            if (item.connectionId == 0) {
                // 唤醒事件：仅用于打断 poller 等待，入队任务已在本轮开始处理。
                continue;
            }
            const auto found = connections.find(item.connectionId);
            if (found == connections.end()) {
                continue;
            }
            Connection& connection = *found->second;
            if ((item.events & POLL_ERROR) != 0) {
                closeConnection(item.connectionId);
                continue;
            }
            if (connection.getState() == ConnectionState::TLS_HANDSHAKE) {
                if ((item.events & POLL_HANGUP) != 0) {
                    closeConnection(item.connectionId);
                } else if ((item.events & (POLL_READ | POLL_WRITE)) != 0) {
                    if ((item.events & POLL_READ) != 0) {
                        connection.onReadable(*executor, 0);
                    } else {
                        connection.onWritable(0);
                    }
                    refreshInterest(item.connectionId);
                }
                continue;
            }
            if ((item.events & (POLL_READ | POLL_HANGUP)) != 0 &&
                connection.getState() != ConnectionState::CLOSED &&
                (connection.getState() != ConnectionState::CLOSING ||
                 connection.pendingTransportOperation != Connection::PendingTransportOperation::NONE)) {
                connection.onReadable(*executor, READ_BUDGET_BYTES);
            }
            if (connection.getState() == ConnectionState::CLOSING && connection.peerClosed) {
                if ((item.events & POLL_WRITE) != 0) {
                    connection.onWritable(WRITE_BUDGET_BYTES);
                }
                refreshInterest(item.connectionId);
                continue;
            }
            if (connection.getState() != ConnectionState::CLOSED && (item.events & POLL_WRITE) != 0) {
                connection.onWritable(WRITE_BUDGET_BYTES);
            }
            refreshInterest(item.connectionId);
        }
    }

    /*
        函数：run
        参数：无
        功能：事件循环主体，处理入队任务和 I/O 事件；收到停止信号后立即退出
        返回：无
    */
    void run() {
        std::array<PollEventItem, POLL_EVENT_BATCH> events{};
        while (!stopping.load(std::memory_order_acquire)) {
            processInbox();
            applyPendingClosures();
            if (stopping.load(std::memory_order_acquire)) {
                break;
            }

            const int count = poller->wait(events.data(), static_cast<int>(events.size()), POLL_WAIT_MS);
            if (count <= 0) {
                continue;
            }
            handleEvents(events.data(), count);
        }

        closeAllConnections();
        discardQueuedTasks();
        running.store(false, std::memory_order_release);
    }
};

Reactor::Reactor(ReactorId id, std::unique_ptr<Poller> poller,
                 std::shared_ptr<ICommandExecutor> executor)
    : impl(std::make_shared<Impl>(id, std::move(poller), std::move(executor))) {}

Reactor::~Reactor() {
    stop();
}

/*
    函数：start
    参数：无
    功能：启动事件线程
    返回：无
*/
void Reactor::start() {
    bool expected = false;
    if (!impl->running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    const auto state = impl;
    if (state->worker.joinable()) {
        // 上一次线程已退出但尚未回收，先接管线程对象再创建新线程。
        state->worker.join();
    }
    state->stopping.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state->inboxMutex);
        state->acceptingPosts = true;
        state->overflowConnections.clear();
    }
    try {
        state->worker = std::thread([state] { state->run(); });
    } catch (...) {
        state->running.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(state->inboxMutex);
        state->acceptingPosts = false;
        throw;
    }
}

/*
    函数：stop
    参数：无
    功能：停止接收新任务、唤醒事件线程并等待连接立即关闭
    返回：无
*/
void Reactor::stop() {
    const auto state = impl;
    {
        std::lock_guard<std::mutex> lock(state->inboxMutex);
        state->stopping.store(true, std::memory_order_release);
        state->acceptingPosts = false;
    }
    state->poller->wakeup();
    if (!state->worker.joinable()) {
        return;
    }
    if (state->worker.get_id() == std::this_thread::get_id()) {
        state->worker.detach();
        return;
    }
    state->worker.join();
}

/*
    函数：post
    参数：task：待执行任务
    功能：把通用任务提交到本 Reactor 执行
    返回：是否提交成功；队列已满或已停止时返回 false，任务被丢弃
*/
bool Reactor::post(ReactorTask task) {
    return impl->enqueue(std::move(task), InboxKind::POST);
}

/*
    函数：addConnection
    参数：connectionId：连接标识；transport：已接受的非阻塞传输；peer：对端地址
          maxPipelineRequests：单连接流水线上限；releaseSlot：客户端槽位释放回调
          maxFrameBytes：单帧上限
    功能：把新连接交给本 Reactor 独占管理
    返回：是否成功提交接入；未提交时已释放槽位并关闭传输
*/
bool Reactor::addConnection(ConnectionId connectionId, std::unique_ptr<Transport> transport,
                            Endpoint peer, std::size_t maxPipelineRequests,
                            ClientSlotRelease releaseSlot, std::uint32_t maxFrameBytes) {
    auto releaseHolder = std::make_shared<SlotRelease>(std::move(releaseSlot));
    if (connectionId == 0 || !transport || maxPipelineRequests == 0 || maxFrameBytes < BASE_HEADER_LENGTH) {
        releaseHolder->run();
        return false;
    }
    auto transportHolder = std::make_shared<std::unique_ptr<Transport>>(std::move(transport));
    const auto state = impl;
    const bool queued = state->enqueue([state, connectionId, transportHolder, peer = std::move(peer),
                                        maxPipelineRequests, releaseHolder, maxFrameBytes]() mutable {
        if (state->connections.find(connectionId) != state->connections.end()) {
            // 连接标识重复：丢弃本次接入，捕获对象负责关闭传输并释放槽位。
            return;
        }
        std::uint64_t generation = state->nextGeneration++;
        if (generation == 0) {
            generation = state->nextGeneration++;
        }

        ConnectionCompletionPoster completionPoster =
            [weakState = std::weak_ptr<Impl>(state), connectionId, generation](CommandResponse response) {
                const auto owner = weakState.lock();
                if (!owner) {
                    return;
                }
                owner->enqueue([weakState, connectionId, generation, response = std::move(response)]() mutable {
                    const auto reactor = weakState.lock();
                    if (!reactor) {
                        return;
                    }
                    const auto found = reactor->connections.find(connectionId);
                    if (found == reactor->connections.end() || found->second->generation != generation) {
                        // 连接已关闭或标识已被复用：丢弃过期完成结果。
                        return;
                    }
                    found->second->complete(std::move(response));
                    reactor->refreshInterest(connectionId);
                }, InboxKind::COMPLETION, connectionId);
            };

        auto connection = std::make_unique<Connection>(connectionId, state->id, std::move(*transportHolder),
            maxPipelineRequests, std::move(peer), maxFrameBytes, generation,
            [releaseHolder]() { releaseHolder->run(); }, std::move(completionPoster));
        if (connection->getState() == ConnectionState::CLOSED) {
            // 构造阶段失败：Connection 已关闭传输并释放槽位。
            return;
        }
        if (!state->poller->add(connectionId, connection->transport->nativeHandle(),
                                connection->interestEvents())) {
            connection->close();
            return;
        }
        connection->pollerRegistered = true;
        state->connections.emplace(connectionId, std::move(connection));
    }, InboxKind::CONTROL);
    if (!queued) {
        releaseHolder->run();
    }
    return queued;
}

/*
    函数：closeConnection
    参数：connectionId：目标连接
    功能：向所属 Reactor 投递立即关闭指定连接的任务
    返回：是否成功提交关闭请求；已停止或队列已满时返回 false
*/
bool Reactor::closeConnection(ConnectionId connectionId) {
    const auto state = impl;
    return state->enqueue([state, connectionId] {
        state->closeConnection(connectionId);
    }, InboxKind::CONTROL);
}

/*
    函数：getId
    参数：无
    功能：读取本 Reactor 标识
    返回：ReactorId
*/
ReactorId Reactor::getId() const noexcept {
    return impl->id;
}

}
