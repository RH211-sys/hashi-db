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

constexpr std::size_t WORKER_COUNT = 4;            // 工作线程数：执行异步存储任务的固定线程数量
constexpr std::size_t MAX_PENDING_TASKS = 1024;    // 待处理上限：限制尚未开始执行的任务数量

/*
    函数：findField
    传参：request：待搜索的结构化请求；id：目标协议字段编号
    功能：在请求字段列表中查找指定字段
    返回值：找到时返回字段地址；未找到时返回空指针
*/
const TlvField* findField(const CommandRequest& request, FieldId id) {
    const auto wanted = fieldId(id);               // 目标字段编号：转换为请求中存储的协议编号
    for (const auto& field : request.fields) {     // 当前字段：逐项查找目标字段编号
        if (field.fieldId == wanted) {
            // 当前字段编号匹配目标编号，返回该字段供调用方读取。
            return &field;
        }
    }
    return nullptr;
}

/*
    函数：keyFrom
    传参：request：包含存储键字段的结构化请求
    功能：从请求中提取键字段并转换为字符串
    返回值：请求键；字段缺失时返回空字符串
*/
std::string keyFrom(const CommandRequest& request) {
    const auto* field = findField(request, FieldId::KEY); // 键字段：取得请求携带的存储键
    if (field == nullptr) {
        // 请求未携带键字段，返回空键以供调用方按缺省语义处理。
        return {};
    }
    return std::string(field->value.begin(), field->value.end());
}

/*
    函数：mapStorageCode
    传参：code：存储层返回的状态码
    功能：将存储层状态转换为网络执行器使用的错误码
    返回值：对应的网络层 ErrorCode
*/
ErrorCode mapStorageCode(int code) {
    if (code == SUCCESS) {
        // 存储操作成功，映射为网络层成功状态。
        return ErrorCode::OK;
    }
    if (code == FIND_FAILED || code == EXPIRED) {
        // 查找失败或数据过期，映射为存储不可用状态。
        return ErrorCode::STORAGE_UNAVAILABLE;
    }
    if (code == TYPE_VALID) {
        // 存储类型未注册，映射为类型未注册状态。
        return ErrorCode::TYPE_NOT_REGISTERED;
    }
    return ErrorCode::INTERNAL;
}

/*
    函数：statusResponse
    传参：request：提供关联标识的原请求；status：响应状态；message：可选响应文本
    功能：构造携带原请求连接和请求标识的状态响应
    返回值：填充后的命令响应
*/
CommandResponse statusResponse(const CommandRequest& request, ErrorCode status, const char* message = nullptr) {
    CommandResponse response;                      // 响应对象：填充请求关联标识和状态
    response.connectionId = request.connectionId;
    response.requestId = request.requestId;
    response.status = status;
    if (message != nullptr) {
        // 调用方提供了说明文本，将其复制到响应中。
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
    std::mutex mutex;                             // 队列互斥量：保护任务队列与停止标记
    std::condition_variable condition;            // 工作通知：唤醒等待任务或停止的线程
    std::deque<std::function<void()>> tasks;       // 有界任务队列：保存待执行的存储操作
    std::vector<std::thread> workers;              // 工作线程：等待并执行队列任务
    bool stopping = false;                         // 停止标记：阻止新任务并唤醒工作线程

public:
    /*
        函数：WorkerPool
        传参：无
        功能：创建固定数量的工作线程并使其等待有界任务队列
        返回值：无
    */
    WorkerPool() {
        workers.reserve(WORKER_COUNT);
        for (std::size_t index = 0; index < WORKER_COUNT; ++index) { // 工作线程下标：创建固定数量的工作线程
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;    // 当前任务：从共享队列取出后在本工作线程执行
                    {
                        std::unique_lock<std::mutex> lock(mutex); // 队列锁：配合条件变量等待并保护任务状态
                        condition.wait(lock, [this] { return stopping || !tasks.empty(); });
                        if (tasks.empty()) {
                            // 当前没有可执行任务，检查线程池是否已经进入停止状态。
                            if (stopping) {
                                // 队列已空且线程池正在停止，结束当前工作线程。
                                return;
                            }
                            // 队列暂时为空但线程池仍运行，回到条件变量等待新任务。
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

    /*
        函数：~WorkerPool
        传参：无
        功能：设置停止标记、唤醒工作线程并等待线程退出
        返回值：无
    */
    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lock(mutex); // 队列锁：保护停止状态和待执行任务队列
            stopping = true;
        }
        condition.notify_all();
        for (auto& worker : workers) {              // 工作线程：等待每个线程完成退出
            worker.join();
        }
    }

    /*
        函数：submit
        传参：task：待在线程池中执行的任务
        功能：在未停止且队列未满时提交任务并唤醒一个工作线程
        返回值：任务是否成功入队
    */
    bool submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex); // 队列锁：保护停止状态和待执行任务队列
            if (stopping || tasks.size() >= MAX_PENDING_TASKS) {
                // 线程池已停止或队列已满，拒绝接收新任务。
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
        // PING 不访问存储，立即返回成功响应。
        completion(statusResponse(request, ErrorCode::OK));
        return;
    }
    if (request.opcode == Opcode::QUIT) {
        // QUIT 返回成功，并要求连接在响应写出后关闭。
        auto response = statusResponse(request, ErrorCode::OK); // 响应对象：构造 QUIT 成功结果并设置写后关闭
        response.closeAfterWrite = true;
        completion(std::move(response));
        return;
    }
    if (request.opcode == Opcode::GET || request.opcode == Opcode::ADD || request.opcode == Opcode::UPDATE) {
        // CRUD 值类型编解码尚未注册，返回类型未注册状态而不提交存储任务。
        completion(statusResponse(request, ErrorCode::TYPE_NOT_REGISTERED,
                                  "wire type codec is not registered"));
        return;
    }

    const std::string key = keyFrom(request);      // 存储键：复制请求键供异步任务使用
    const ConnectionId connectionId = request.connectionId; // 连接标识：用于线程池拒绝时构造关联响应
    const RequestId requestId = request.requestId; // 请求标识：用于线程池拒绝时构造关联响应
    auto completionHolder = std::make_shared<CommandCompletion>(std::move(completion)); // 完成回调：在调用方与工作任务间共享
    const bool accepted = workerPool->submit([this, request = std::move(request), key = std::move(key),
                                              completionHolder]() mutable { // request、key、completionHolder：任务请求、存储键和完成回调
        CommandResponse response;                  // 响应对象：保存存储任务结果和原请求关联标识
        response.connectionId = request.connectionId;
        response.requestId = request.requestId;
        try {
            int result = UNKNOWN_ERROR;             // 存储结果：接收 Controller 异步操作返回码
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
                // 存储操作未成功，附加统一的失败说明。
                response.message = "storage operation failed";
            }
        } catch (...) {
            response.status = ErrorCode::STORAGE_UNAVAILABLE;
            response.message = "storage operation unavailable";
        }
        (*completionHolder)(std::move(response));
    }); // 接收结果：标记任务是否成功提交到有界工作队列

    if (!accepted) {
        // 工作队列拒绝任务，使用原请求标识返回繁忙响应。
        CommandResponse response;                  // 拒绝响应：保持原请求关联信息并报告队列繁忙
        response.connectionId = connectionId;
        response.requestId = requestId;
        response.status = ErrorCode::BUSY;
        response.message = "storage executor queue is full";
        (*completionHolder)(std::move(response));
    }
}

}
