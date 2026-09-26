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
    地位：接入任务与连接对象之间的一次性连接槽位释放守卫。
    功能：保证连接槽位释放回调在所有持有者之间只执行一次。
        连接接入任务可能因队列满被丢弃，此时也必须释放已占用的客户端槽位。
*/
struct SlotRelease {
    std::atomic<bool> released{false};             // 释放标记：保证回调只执行一次
    ClientSlotRelease callback;                    // 槽位释放回调：由 Acceptor 提供

    /*
        功能：保存客户端槽位释放回调
        传参：callback：客户端槽位释放动作
        返回值：无
    */
    explicit SlotRelease(ClientSlotRelease callback) : callback(std::move(callback)) {}

    /*
        函数：run
        传参：无
        功能：执行一次槽位释放回调
        返回值：无
    */
    void run() {
        if (!released.exchange(true, std::memory_order_acq_rel) && callback) {
            // 首次触发释放且回调有效，执行回调归还客户端槽位。
            callback();
        }
    }

    /*
        功能：析构时确保客户端槽位释放回调已执行
        传参：无
        返回值：无
    */
    ~SlotRelease() {
        run();
    }
};

}

/*
    地位：Reactor 对外句柄背后的事件循环共享状态。
    功能：保存连接表、轮询器、任务队列及事件线程所需状态。
*/
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

    /*
        功能：初始化 Reactor 标识、事件轮询器和命令执行器
        传参：reactorId：Reactor 标识；reactorPoller：事件轮询器；commandExecutor：命令执行器
        返回值：无
    */
    Impl(ReactorId reactorId, std::unique_ptr<Poller> reactorPoller,
         std::shared_ptr<ICommandExecutor> commandExecutor)
        : id(reactorId), poller(std::move(reactorPoller)), executor(std::move(commandExecutor)) {}

    /*
        函数：enqueue
        传参：task：待执行任务；kind：队列用途；connectionId：完成结果所属连接
        功能：按用途把任务放入对应有界队列，队列已满时按既定规则丢弃
        返回值：任务是否成功入队
        备注：完成结果队列满时放弃该响应，并请求关闭对应连接，避免连接停留在等待完成的状态
    */
    bool enqueue(ReactorTask task, InboxKind kind, ConnectionId connectionId = 0) {
        bool accepted = false;                     // 入队结果：标记任务是否已接受
        bool needWakeup = false;                   // 唤醒标记：有新任务时通知轮询线程
        {
            std::lock_guard<std::mutex> lock(inboxMutex); // 队列锁：保护全部收件队列和入队状态
            if (!running.load(std::memory_order_acquire)) {
                // Reactor 尚未运行，拒绝接收任务。
                return false;
            }
            const bool stoppingNow = stopping.load(std::memory_order_acquire); // 停止快照：判断当前入队是否允许
            if (kind == InboxKind::COMPLETION) {
                // 当前任务是业务完成结果，使用独立完成队列和溢出处理规则。
                if (stoppingNow) {
                    // Reactor 已进入停止阶段，不再接收新的业务完成结果。
                    return false;
                }
                if (completionInbox.size() < MAX_COMPLETION_TASKS) {
                    // 完成队列仍有容量，保存结果并请求唤醒事件线程。
                    completionInbox.push_back(std::move(task));
                    accepted = true;
                    needWakeup = true;
                } else if (connectionId != 0) {
                    // 完成队列已满且连接标识有效，记录该连接以便事件线程关闭。
                    overflowConnections.insert(connectionId);
                    needWakeup = true;
                }
            } else if (!acceptingPosts || stoppingNow) {
                // Reactor 不再接收普通或控制任务，拒绝当前任务。
                return false;
            } else if (kind == InboxKind::CONTROL) {
                // 当前任务是连接控制任务，使用独立控制队列。
                if (controlInbox.size() < MAX_CONTROL_TASKS) {
                    // 控制队列仍有容量，保存任务并请求唤醒事件线程。
                    controlInbox.push_back(std::move(task));
                    accepted = true;
                    needWakeup = true;
                }
            } else if (inbox.size() < MAX_POST_TASKS) {
                // 当前任务是通用任务且队列有容量，保存任务并请求唤醒事件线程。
                inbox.push_back(std::move(task));
                accepted = true;
                needWakeup = true;
            }
        }
        if (needWakeup) {
            // 已接收任务或记录待关闭连接，唤醒事件线程处理队列变化。
            poller->wakeup();
        }
        return accepted;
    }

    /*
        函数：processInbox
        传参：无
        功能：取出当前全部待执行任务并在本线程依次执行，单个任务异常不影响事件循环
        返回值：无
    */
    void processInbox() {
        std::deque<ReactorTask> tasks;             // 待执行任务：暂存本轮从各入口队列取出的任务
        {
            std::lock_guard<std::mutex> lock(inboxMutex); // 队列锁：保护待处理任务的取出操作
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
        for (auto& task : tasks) {                 // 当前任务：按取出顺序交由事件线程执行
            if (stopping.load(std::memory_order_acquire)) {
                // Reactor 已收到停止信号，停止执行本批剩余任务。
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
        传参：无
        功能：关闭因完成结果被丢弃而标记的连接
        返回值：无
    */
    void applyPendingClosures() {
        std::vector<ConnectionId> pending;         // 待关闭连接：暂存完成队列溢出时记录的连接标识
        {
            std::lock_guard<std::mutex> lock(inboxMutex); // 队列锁：保护待关闭连接集合
            if (overflowConnections.empty()) {
                // 没有因完成结果溢出而待关闭的连接，结束本次处理。
                return;
            }
            pending.assign(overflowConnections.begin(), overflowConnections.end());
            overflowConnections.clear();
        }
        for (ConnectionId connectionId : pending) { // 连接标识：逐个关闭无法接收完成结果的连接
            closeConnection(connectionId);
        }
    }

    /*
        函数：discardQueuedTasks
        传参：无
        功能：丢弃全部未执行任务，并重置入队状态
        返回值：无
        备注：接入任务持有连接槽位，丢弃时由捕获对象释放；本线程退出前调用
    */
    void discardQueuedTasks() {
        std::deque<ReactorTask> discarded;         // 待丢弃任务：接收停止时尚未执行的任务
        {
            std::lock_guard<std::mutex> lock(inboxMutex); // 队列锁：保护任务队列与接收状态
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
        传参：connectionId：目标连接
        功能：按连接当前状态刷新 Poller 关注事件，连接已关闭或刷新失败时关闭连接
        返回值：无
    */
    void refreshInterest(ConnectionId connectionId) {
        const auto found = connections.find(connectionId); // 连接项：查找兴趣状态对应的连接
        if (found == connections.end()) {
            // 连接已不在本 Reactor 管理表中，无需刷新其事件。
            return;
        }
        if (found->second->getState() == ConnectionState::CLOSED) {
            // 连接已关闭，从 Poller 和连接表中清理该连接。
            closeConnection(connectionId);
            return;
        }
        const std::uint32_t events = found->second->interestEvents(); // 关注事件：连接当前需要监听的事件掩码
        if (found->second->getState() == ConnectionState::CLOSING &&
            found->second->peerClosed && events == 0) {
            // 对端已半关闭且不再关注读写事件，从 Poller 中移除该连接。
            if (found->second->pollerRegistered) {
                // 连接当前已注册到 Poller，移除监听并更新登记状态。
                poller->remove(connectionId);
                found->second->pollerRegistered = false;
            }
            return;
        }
        if (events == 0) {
            // 当前没有需要监听的事件，移除已登记项后结束刷新。
            if (found->second->pollerRegistered) {
                // 连接仍在 Poller 登记表中，移除监听并更新登记状态。
                poller->remove(connectionId);
                found->second->pollerRegistered = false;
            }
            return;
        }
        const bool updated = found->second->pollerRegistered
            ? poller->modify(connectionId, events)
            : poller->add(connectionId, found->second->transport->nativeHandle(), events); // 更新结果：记录轮询器注册或修改是否成功
        if (!updated) {
            // Poller 添加或修改监听失败，关闭无法继续驱动的连接。
            closeConnection(connectionId);
            return;
        }
        found->second->pollerRegistered = true;
    }

    /*
        函数：closeConnection
        传参：connectionId：目标连接
        功能：从 Poller 和连接表移除连接，释放 socket 与客户端槽位
        返回值：无
        备注：只能在事件线程调用，且调用期间不得持有 inboxMutex
    */
    void closeConnection(ConnectionId connectionId) {
        const auto found = connections.find(connectionId); // 连接项：定位待关闭的连接对象
        if (found == connections.end()) {
            // 连接已不在本 Reactor 管理表中，无需重复关闭。
            return;
        }
        if (found->second->pollerRegistered) {
            // 连接已登记到 Poller，先移除事件监听并更新登记状态。
            poller->remove(connectionId);
            found->second->pollerRegistered = false;
        }
        found->second->close();
        connections.erase(found);
        std::lock_guard<std::mutex> lock(inboxMutex); // 队列锁：保护待关闭连接集合
        overflowConnections.erase(connectionId);
    }

    /*
        函数：closeAllConnections
        传参：无
        功能：关闭当前全部连接
        返回值：无
    */
    void closeAllConnections() {
        while (!connections.empty()) {
            closeConnection(connections.begin()->first);
        }
    }

    /*
        函数：handleEvents
        传参：events：本次轮询结果；count：有效事件数量
        功能：按事件驱动对应连接的读写、握手与关闭
        返回值：无
    */
    void handleEvents(const PollEventItem* events, int count) {
        for (int index = 0; index < count; ++index) { // 事件下标：依次处理本轮轮询返回的事件
            const PollEventItem item = events[index]; // 当前事件：包含连接标识和就绪事件掩码
            if (item.connectionId == 0) {
                // 唤醒事件：仅用于打断 poller 等待，入队任务已在本轮开始处理。
                continue;
            }
            const auto found = connections.find(item.connectionId); // 连接项：定位当前事件所属连接
            if (found == connections.end()) {
                // 事件对应的连接已被移除，忽略过期事件。
                continue;
            }
            Connection& connection = *found->second; // 连接对象：处理当前就绪事件的目标连接
            if ((item.events & POLL_ERROR) != 0) {
                // Poller 报告连接错误，立即关闭该连接并跳过其余事件处理。
                closeConnection(item.connectionId);
                continue;
            }
            if (connection.getState() == ConnectionState::TLS_HANDSHAKE) {
                // 连接正在执行 TLS 握手，仅按握手所需事件推进状态。
                if ((item.events & POLL_HANGUP) != 0) {
                    // 握手期间对端挂断，关闭连接并跳过后续处理。
                    closeConnection(item.connectionId);
                } else if ((item.events & (POLL_READ | POLL_WRITE)) != 0) {
                    // 握手期间出现读写就绪事件，按读优先顺序推进握手。
                    if ((item.events & POLL_READ) != 0) {
                        // 当前事件包含可读标记，通过可读回调推进握手。
                        connection.onReadable(*executor, 0);
                    } else {
                        // 当前仅有可写标记，通过可写回调推进握手。
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
                // 连接存在读或挂断事件且仍需处理输入，按预算推进读取。
                connection.onReadable(*executor, READ_BUDGET_BYTES);
            }
            if (connection.getState() == ConnectionState::CLOSING && connection.peerClosed) {
                // 连接正排空对端半关闭后的响应，只处理写事件并刷新监听状态。
                if ((item.events & POLL_WRITE) != 0) {
                    // 当前事件可写，继续发送排队响应。
                    connection.onWritable(WRITE_BUDGET_BYTES);
                }
                refreshInterest(item.connectionId);
                continue;
            }
            if (connection.getState() != ConnectionState::CLOSED && (item.events & POLL_WRITE) != 0) {
                // 连接仍有效且出现可写事件，按预算继续发送响应。
                connection.onWritable(WRITE_BUDGET_BYTES);
            }
            refreshInterest(item.connectionId);
        }
    }

    /*
        函数：run
        传参：无
        功能：事件循环主体，处理入队任务和 I/O 事件；收到停止信号后立即退出
        返回值：无
    */
    void run() {
        std::array<PollEventItem, POLL_EVENT_BATCH> events{}; // 事件批次：接收 Poller 返回的就绪事件
        while (!stopping.load(std::memory_order_acquire)) {
            processInbox();
            applyPendingClosures();
            if (stopping.load(std::memory_order_acquire)) {
                // 停止信号已生效，退出事件循环并进入统一清理流程。
                break;
            }

            const int count = poller->wait(events.data(), static_cast<int>(events.size()), POLL_WAIT_MS); // 事件数量：本轮轮询返回的有效事件数
            if (count <= 0) {
                // 本轮无事件或轮询失败，返回循环顶部处理任务和停止状态。
                continue;
            }
            handleEvents(events.data(), count);
        }

        closeAllConnections();
        discardQueuedTasks();
        running.store(false, std::memory_order_release);
    }
};

/*
    功能：创建 Reactor 并初始化其共享实现状态
    传参：id：Reactor 标识；poller：事件轮询器；executor：命令执行器
    返回值：无
*/
Reactor::Reactor(ReactorId id, std::unique_ptr<Poller> poller,
                 std::shared_ptr<ICommandExecutor> executor)
    : impl(std::make_shared<Impl>(id, std::move(poller), std::move(executor))) {}

/*
    功能：销毁 Reactor 前停止并回收事件线程
    传参：无
    返回值：无
*/
Reactor::~Reactor() {
    stop();
}

/*
    函数：start
    传参：无
    功能：启动事件线程
    返回值：无
*/
void Reactor::start() {
    bool expected = false;                         // 期望运行态：原子启动前应为未运行
    if (!impl->running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        // Reactor 已处于运行状态，避免重复启动线程。
        return;
    }
    const auto state = impl;                       // 实现快照：保持异步事件线程期间的状态对象存活
    if (state->worker.joinable()) {
        // 上一次线程已退出但尚未回收，先接管线程对象再创建新线程。
        state->worker.join();
    }
    state->stopping.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(state->inboxMutex); // 队列锁：开启任务接收并清理旧溢出标记
        state->acceptingPosts = true;
        state->overflowConnections.clear();
    }
    try {
        state->worker = std::thread([state] { state->run(); });
    } catch (...) {
        state->running.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(state->inboxMutex); // 队列锁：启动失败后关闭任务接收
        state->acceptingPosts = false;
        throw;
    }
}

/*
    函数：stop
    传参：无
    功能：停止接收新任务、唤醒事件线程并等待连接立即关闭
    返回值：无
*/
void Reactor::stop() {
    const auto state = impl;                       // 实现快照：保持停止流程访问状态对象期间其存活
    {
        std::lock_guard<std::mutex> lock(state->inboxMutex); // 队列锁：同步停止标记与任务接收状态
        state->stopping.store(true, std::memory_order_release);
        state->acceptingPosts = false;
    }
    state->poller->wakeup();
    if (!state->worker.joinable()) {
        // 没有可回收的事件线程，停止操作已完成。
        return;
    }
    if (state->worker.get_id() == std::this_thread::get_id()) {
        // 当前线程就是事件线程，分离线程避免自我等待造成死锁。
        state->worker.detach();
        return;
    }
    state->worker.join();
}

/*
    函数：post
    传参：task：待执行任务
    功能：把通用任务提交到本 Reactor 执行
    返回值：是否提交成功；队列已满或已停止时返回 false，任务被丢弃
*/
bool Reactor::post(ReactorTask task) {
    return impl->enqueue(std::move(task), InboxKind::POST);
}

/*
    函数：addConnection
    传参：connectionId：连接标识；transport：已接受的非阻塞传输；peer：对端地址
          maxPipelineRequests：单连接流水线上限；releaseSlot：客户端槽位释放回调
          maxFrameBytes：单帧上限
    功能：把新连接交给本 Reactor 独占管理
    返回值：是否成功提交接入；未提交时已释放槽位并关闭传输
*/
bool Reactor::addConnection(ConnectionId connectionId, std::unique_ptr<Transport> transport,
                            Endpoint peer, std::size_t maxPipelineRequests,
                            ClientSlotRelease releaseSlot, std::uint32_t maxFrameBytes) {
    auto releaseHolder = std::make_shared<SlotRelease>(std::move(releaseSlot)); // 槽位释放器：由接入任务和连接共同持有
    if (connectionId == 0 || !transport || maxPipelineRequests == 0 || maxFrameBytes < BASE_HEADER_LENGTH) {
        // 接入参数不满足连接创建要求，释放槽位并拒绝该连接。
        releaseHolder->run();
        return false;
    }
    auto transportHolder = std::make_shared<std::unique_ptr<Transport>>(std::move(transport)); // 传输持有者：确保排队期间传输对象生命周期有效
    const auto state = impl;                       // 实现快照：供异步接入任务和完成回调安全访问
    // 入队结果：标记连接创建控制任务是否成功提交。
    const bool queued = state->enqueue([state, connectionId, transportHolder, peer = std::move(peer),
                                        maxPipelineRequests, releaseHolder, maxFrameBytes]() mutable {
        if (state->connections.find(connectionId) != state->connections.end()) {
            // 连接标识重复：丢弃本次接入，捕获对象负责关闭传输并释放槽位。
            return;
        }
        std::uint64_t generation = state->nextGeneration++; // 连接代次：区分同一连接标识的不同生命周期
        if (generation == 0) {
            // 代次递增发生回绕，跳过保留的零代次。
            generation = state->nextGeneration++;
        }

        ConnectionCompletionPoster completionPoster = // 完成回调：将业务结果按连接代次投递回本 Reactor
            [weakState = std::weak_ptr<Impl>(state), connectionId, generation](CommandResponse response) { // response：业务层返回的请求完成结果
                const auto owner = weakState.lock(); // Reactor 状态：完成回调可访问时所属的事件循环
                if (!owner) {
                    // Reactor 已销毁，无法安全接收该业务完成结果。
                    return;
                }
                owner->enqueue([weakState, connectionId, generation, response = std::move(response)]() mutable { // response：待按连接代次投递的业务完成结果
                    const auto reactor = weakState.lock(); // Reactor 状态：完成结果所属的事件循环
                    if (!reactor) {
                        // Reactor 已销毁，丢弃无法回投的完成结果。
                        return;
                    }
                    const auto found = reactor->connections.find(connectionId); // 连接项：校验连接仍存在且代次匹配
                    if (found == reactor->connections.end() || found->second->generation != generation) {
                        // 连接已关闭或标识已被复用：丢弃过期完成结果。
                        return;
                    }
                    found->second->complete(std::move(response));
                    reactor->refreshInterest(connectionId);
                }, InboxKind::COMPLETION, connectionId);
            };

        auto connection = std::make_unique<Connection>(connectionId, state->id, std::move(*transportHolder), // connection：绑定当前 Reactor 的新连接对象
            maxPipelineRequests, std::move(peer), maxFrameBytes, generation,
            [releaseHolder]() { releaseHolder->run(); }, std::move(completionPoster));
        if (connection->getState() == ConnectionState::CLOSED) {
            // 构造阶段失败：Connection 已关闭传输并释放槽位。
            return;
        }
        if (!state->poller->add(connectionId, connection->transport->nativeHandle(),
                                connection->interestEvents())) {
            // Poller 注册新连接失败，关闭连接并释放其资源。
            connection->close();
            return;
        }
        connection->pollerRegistered = true;
        state->connections.emplace(connectionId, std::move(connection));
    }, InboxKind::CONTROL);
    if (!queued) {
        // 控制任务未能入队，立即释放已预留的客户端槽位。
        releaseHolder->run();
    }
    return queued;
}

/*
    函数：closeConnection
    传参：connectionId：目标连接
    功能：向所属 Reactor 投递立即关闭指定连接的任务
    返回值：是否成功提交关闭请求；已停止或队列已满时返回 false
*/
bool Reactor::closeConnection(ConnectionId connectionId) {
    const auto state = impl;                       // 实现快照：供排队的关闭任务访问 Reactor 状态
    return state->enqueue([state, connectionId] {
        state->closeConnection(connectionId);
    }, InboxKind::CONTROL);
}

/*
    函数：getId
    传参：无
    功能：读取本 Reactor 标识
    返回值：ReactorId
*/
ReactorId Reactor::getId() const noexcept {
    return impl->id;
}

}
