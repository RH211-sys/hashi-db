#include "epoll_poller.h"

#include <cerrno>
#include <limits>

#if defined(__linux__) && !defined(_WIN32)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace mydb::net {
namespace {

#if defined(__linux__) && !defined(_WIN32)
std::uint32_t toNativeEvents(std::uint32_t events) {
    std::uint32_t result = 0;
    if ((events & POLL_READ) != 0) {
        result |= EPOLLIN;
    }
    if ((events & POLL_WRITE) != 0) {
        result |= EPOLLOUT;
    }
    return result;
}

std::uint32_t fromNativeEvents(std::uint32_t events) {
    std::uint32_t result = 0;
    if ((events & EPOLLIN) != 0) {
        result |= POLL_READ;
    }
    if ((events & EPOLLOUT) != 0) {
        result |= POLL_WRITE;
    }
    if ((events & EPOLLERR) != 0) {
        result |= POLL_ERROR;
    }
    if ((events & (EPOLLHUP | EPOLLRDHUP)) != 0) {
        result |= POLL_HANGUP;
    }
    return result;
}

int epollControl(int epollFd, int operation, int fd, epoll_event* event) {
    int result;
    do {
        result = ::epoll_ctl(epollFd, operation, fd, event);
    } while (result == -1 && errno == EINTR);
    return result;
}
#endif

}

EpollPoller::EpollPoller() noexcept {
#if defined(__linux__) && !defined(_WIN32)
    epollHandle = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollHandle == -1) {
        return;
    }

    wakeHandle = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeHandle == -1) {
        ::close(epollHandle);
        epollHandle = -1;
        return;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = 0;
    if (epollControl(epollHandle, EPOLL_CTL_ADD, wakeHandle, &event) == -1) {
        ::close(wakeHandle);
        ::close(epollHandle);
        wakeHandle = -1;
        epollHandle = -1;
    }
#endif
}

EpollPoller::~EpollPoller() {
#if defined(__linux__) && !defined(_WIN32)
    if (wakeHandle != -1) {
        ::close(wakeHandle);
    }
    if (epollHandle != -1) {
        ::close(epollHandle);
    }
#endif
}

bool EpollPoller::add(ConnectionId connectionId, std::intptr_t nativeHandle, std::uint32_t events) {
#if defined(__linux__) && !defined(_WIN32)
    if (epollHandle == -1 || connectionId == 0 || nativeHandle < 0 ||
        nativeHandle > std::numeric_limits<int>::max() || descriptors.contains(connectionId)) {
        return false;
    }

    const auto [entry, inserted] = descriptors.emplace(connectionId, nativeHandle);
    if (!inserted) {
        return false;
    }

    epoll_event event{};
    event.events = toNativeEvents(events) | EPOLLRDHUP;
    event.data.u64 = connectionId;
    if (epollControl(epollHandle, EPOLL_CTL_ADD, static_cast<int>(nativeHandle), &event) == -1) {
        descriptors.erase(entry);
        return false;
    }
    return true;
#else
    (void)connectionId;
    (void)nativeHandle;
    (void)events;
    return false;
#endif
}

bool EpollPoller::modify(ConnectionId connectionId, std::uint32_t events) {
#if defined(__linux__) && !defined(_WIN32)
    if (epollHandle == -1 || connectionId == 0) {
        return false;
    }
    const auto entry = descriptors.find(connectionId);
    if (entry == descriptors.end()) {
        return false;
    }

    epoll_event event{};
    event.events = toNativeEvents(events) | EPOLLRDHUP;
    event.data.u64 = connectionId;
    return epollControl(epollHandle, EPOLL_CTL_MOD, static_cast<int>(entry->second), &event) == 0;
#else
    (void)connectionId;
    (void)events;
    return false;
#endif
}

bool EpollPoller::remove(ConnectionId connectionId) {
#if defined(__linux__) && !defined(_WIN32)
    if (epollHandle == -1 || connectionId == 0) {
        return false;
    }
    const auto entry = descriptors.find(connectionId);
    if (entry == descriptors.end()) {
        return false;
    }

    const int fd = static_cast<int>(entry->second);
    if (epollControl(epollHandle, EPOLL_CTL_DEL, fd, nullptr) == 0 || errno == ENOENT || errno == EBADF) {
        descriptors.erase(entry);
        return true;
    }
    return false;
#else
    (void)connectionId;
    return false;
#endif
}

int EpollPoller::wait(PollEventItem* events, int maxEvents, int timeoutMs) {
#if defined(__linux__) && !defined(_WIN32)
    if (epollHandle == -1 || events == nullptr || maxEvents <= 0) {
        return -1;
    }
    epoll_event nativeEvents[128];
    const int capacity = maxEvents < 128 ? maxEvents : 128;
    int count;
    do {
        count = ::epoll_wait(epollHandle, nativeEvents, capacity, timeoutMs);
    } while (count == -1 && errno == EINTR);
    if (count == -1) {
        return -1;
    }

    int outputCount = 0;
    for (int i = 0; i < count; ++i) {
        const ConnectionId id = nativeEvents[i].data.u64;
        if (id == 0) {
            std::uint64_t value;
            ssize_t readCount;
            do {
                readCount = ::read(wakeHandle, &value, sizeof(value));
            } while (readCount == -1 && errno == EINTR);
            if (readCount == static_cast<ssize_t>(sizeof(value)) ||
                (readCount == -1 && errno == EAGAIN)) {
                events[outputCount++] = PollEventItem{0, POLL_WAKE};
            } else {
                return -1;
            }
            continue;
        }
        events[outputCount++] = PollEventItem{id, fromNativeEvents(nativeEvents[i].events)};
    }
    return outputCount;
#else
    (void)events;
    (void)maxEvents;
    (void)timeoutMs;
    return -1;
#endif
}

bool EpollPoller::wakeup() {
#if defined(__linux__) && !defined(_WIN32)
    if (wakeHandle == -1) {
        return false;
    }
    const std::uint64_t value = 1;
    ssize_t written;
    do {
        written = ::write(wakeHandle, &value, sizeof(value));
    } while (written == -1 && errno == EINTR);
    return written == static_cast<ssize_t>(sizeof(value)) || (written == -1 && errno == EAGAIN);
#else
    return false;
#endif
}

}
