/*
    模块名：网络命令执行适配
    模块地位：网络层到业务执行层的阶段性适配
    模块功能描述：在线程池工作线程等待存储 future，并将结果转换为网络层响应。
*/

#include "command_executor.h"
#include "protocol_fields.h"

#include "../../data_memo_level/controller/controller.h"
#include "../../data_memo_level/protocol.h"

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mydb::net {
namespace {

constexpr std::size_t WORKER_COUNT = 4;
constexpr std::size_t MAX_PENDING_TASKS = 1024;

const TlvField* findField(const CommandRequest& request, FieldId id) {
    const auto wanted = fieldId(id);
    for (const auto& field : request.fields) {
        if (field.fieldId == wanted) {
            return &field;
        }
    }
    return nullptr;
}

std::string keyFrom(const CommandRequest& request) {
    const auto* field = findField(request, FieldId::KEY);
    if (field == nullptr) {
        return {};
    }
    return std::string(field->value.begin(), field->value.end());
}

ErrorCode mapStorageCode(int code) {
    if (code == SUCCESS) {
        return ErrorCode::OK;
    }
    if (code == FIND_FAILED || code == EXPIRED) {
        return ErrorCode::STORAGE_UNAVAILABLE;
    }
    if (code == TYPE_VALID) {
        return ErrorCode::TYPE_NOT_REGISTERED;
    }
    return ErrorCode::INTERNAL;
}

CommandResponse statusResponse(const CommandRequest& request, ErrorCode status, const char* message = nullptr) {
    CommandResponse response;
    response.connectionId = request.connectionId;
    response.requestId = request.requestId;
    response.status = status;
    if (message != nullptr) {
        response.message = message;
    }
    return response;
}

} // namespace

/*
    地位：网络执行器的异步等待设施
    功能：在线程池工作线程等待存储 future，避免网络 I/O 线程阻塞。
*/
class StorageControllerExecutor::WorkerPool {
private:
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::function<void()>> tasks;
    std::vector<std::thread> workers;
    bool stopping = false;

public:
    WorkerPool() {
        workers.reserve(WORKER_COUNT);
        for (std::size_t index = 0; index < WORKER_COUNT; ++index) {
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        condition.wait(lock, [this] { return stopping || !tasks.empty(); });
                        if (tasks.empty()) {
                            if (stopping) {
                                return;
                            }
                            continue;
                        }
                        task = std::move(tasks.front());
                        tasks.pop_front();
                    }
                    try {
                        task();
                    } catch (...) {
                    }
                }
            });
        }
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        condition.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }

    bool submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || tasks.size() >= MAX_PENDING_TASKS) {
                return false;
            }
            tasks.push_back(std::move(task));
        }
        condition.notify_one();
        return true;
    }
};

StorageControllerExecutor::StorageControllerExecutor(::Controller& storageController)
    : storageController(storageController), workerPool(std::make_unique<WorkerPool>()) {}

StorageControllerExecutor::~StorageControllerExecutor() = default;

void StorageControllerExecutor::execute(CommandRequest request, CommandCompletion completion) {

    if (request.opcode == Opcode::PING) {
        completion(statusResponse(request, ErrorCode::OK));
        return;
    }
    if (request.opcode == Opcode::QUIT) {
        auto response = statusResponse(request, ErrorCode::OK);
        response.closeAfterWrite = true;
        completion(std::move(response));
        return;
    }
    if (request.opcode == Opcode::GET || request.opcode == Opcode::ADD || request.opcode == Opcode::UPDATE) {
        completion(statusResponse(request, ErrorCode::TYPE_NOT_REGISTERED,
                                  "wire type codec is not registered"));
        return;
    }

    const std::string key = keyFrom(request);
    CommandRequest pendingRequest = request;
    auto completionHolder = std::make_shared<CommandCompletion>(std::move(completion));
    const bool accepted = workerPool->submit([this, request = std::move(request), key = std::move(key),
                                              completionHolder]() mutable {
        CommandResponse response;
        response.connectionId = request.connectionId;
        response.requestId = request.requestId;
        try {
            int result = UNKNOWN_ERROR;
            switch (request.opcode) {
            case Opcode::DELETE_DATA:
                result = storageController.delData(key).get();
                break;
            case Opcode::PERSIST:
                result = key.empty() ? storageController.persisAll().get()
                                     : storageController.persisVar(key).get();
                break;
            case Opcode::FLUSH:
                result = storageController.flushDisk().get();
                break;
            case Opcode::REWRITE:
                result = storageController.reWrite().get();
                break;
            default:
                response.status = ErrorCode::UNSUPPORTED;
                response.message = "command is not implemented by storage bridge";
                (*completionHolder)(std::move(response));
                return;
            }
            response.status = mapStorageCode(result);
            if (response.status != ErrorCode::OK) {
                response.message = "storage operation failed";
            }
        } catch (...) {
            response.status = ErrorCode::STORAGE_UNAVAILABLE;
            response.message = "storage operation unavailable";
        }
        (*completionHolder)(std::move(response));
    });

    if (!accepted) {
        (*completionHolder)(statusResponse(pendingRequest, ErrorCode::BUSY, "storage executor queue is full"));
    }
}

}
