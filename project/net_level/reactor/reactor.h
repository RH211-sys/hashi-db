#pragma once
#ifndef _MYDB_NET_REACTOR_H_
#define _MYDB_NET_REACTOR_H_

/*
    模块名：Reactor 事件循环
    功能描述：在单线程内驱动 Poller、Connection 和跨线程 inbox，保证连接线程亲和性。
*/

#include "poller.h"
#include "../command/command_executor.h"
#include "../protocol/protocol.h"
#include "../server/acceptor.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace mydb::net {

class Connection;
using ReactorTask = std::function<void()>;

class Reactor {
private:
    struct Impl;
    std::shared_ptr<Impl> impl;

public:
    Reactor(ReactorId id, std::unique_ptr<Poller> poller, std::shared_ptr<ICommandExecutor> executor);
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;
    Reactor& operator=(Reactor&&) = delete;

    void start();
    void stop();
    bool post(ReactorTask task);
    bool addConnection(ConnectionId connectionId, std::unique_ptr<Transport> transport, Endpoint peer,
                       std::size_t maxPipelineRequests, ClientSlotRelease releaseSlot = {},
                       std::uint32_t maxFrameBytes = DEFAULT_MAX_FRAME_BYTES);
    bool closeConnection(ConnectionId connectionId);
    ReactorId getId() const noexcept;
};

}

#endif
