/*
    模块名：网络层连接流水线压力测试
    模块地位：验证协议解析、连接排队、异步完成和响应编码的持续处理能力
    模块功能描述：在内存传输上顺序提交大量 PING 请求，统计完成速率并校验响应关联。
*/

#include "net_test_support.h"

#include "net_level/server/connection.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace mydb::net;
using namespace mydb::net::test;

namespace {

constexpr std::size_t STRESS_REQUEST_COUNT = 100000;
constexpr std::size_t CONNECTION_READ_BUDGET = 16U * 1024U * 1024U;
constexpr std::size_t RESPONSE_WRITE_BUDGET = 64;

/*
    地位：连接压力测试的可控异步执行器
    功能：每次只持有一个业务完成回调，由测试循环逐个完成并记录提交量。
*/
class PendingExecutor final : public ICommandExecutor {
private:
    std::optional<CommandCompletion> pendingCompletion; // 待完成回调：模拟异步业务结果
    ConnectionId pendingConnectionId = 0;                // 连接标识：写回当前响应
    RequestId pendingRequestId = 0;                      // 请求标识：写回当前响应
    std::size_t submittedCount = 0;                      // 提交计数：已进入执行边界的请求数

public:
    /*
        功能：记录单个请求并保存其异步完成回调
        传参：request：结构化命令；completion：网络层完成回调
        返回值：无
    */
    void execute(CommandRequest request, CommandCompletion completion) override {
        require(!pendingCompletion.has_value(), "connection submits at most one request at a time");
        pendingConnectionId = request.connectionId;
        pendingRequestId = request.requestId;
        pendingCompletion = std::move(completion);
        ++submittedCount;
    }

    /*
        功能：完成当前待处理请求
        传参：无
        返回值：无
    */
    void completePending() {
        require(pendingCompletion.has_value(), "stress executor has a pending request");
        CommandCompletion completion = std::move(*pendingCompletion);
        pendingCompletion.reset();
        CommandResponse response;
        response.connectionId = pendingConnectionId;
        response.requestId = pendingRequestId;
        completion(std::move(response));
    }

    /*
        功能：读取累计提交请求数
        传参：无
        返回值：提交请求总数
    */
    std::size_t submittedRequests() const {
        return submittedCount;
    }

    /*
        功能：查询是否仍有未完成请求
        传参：无
        返回值：是否存在待完成回调
    */
    bool hasPendingRequest() const {
        return pendingCompletion.has_value();
    }
};

/*
    功能：构造包含指定数量请求的连续网络输入
    传参：requestCount：请求数量
    返回值：编码后的连续帧字节流
*/
ByteBuffer makeStressInput(std::size_t requestCount) {
    ByteBuffer input;
    input.reserve(requestCount * BASE_HEADER_LENGTH);
    FrameCodec encoder;
    for (std::size_t index = 0; index < requestCount; ++index) {
        const Frame request = makeFrame(Opcode::PING, index + 1);
        const ByteBuffer encoded = encoder.encode(request);
        require(!encoded.empty(), "encode stress request");
        input.insert(input.end(), encoded.begin(), encoded.end());
    }
    return input;
}

/*
    功能：逐个解析并校验连接输出的所有关联响应
    传参：output：连续响应字节流；expectedCount：预期响应数量
    返回值：無
*/
void verifyStressResponses(const ByteBuffer& output, std::size_t expectedCount) {
    FrameCodec decoder;
    std::size_t offset = 0;
    std::size_t decodedCount = 0;
    while (offset < output.size()) {
        Frame response;
        require(decoder.feed(output, offset, response) == DecodeStatus::FRAME_READY,
                "decode stress response stream");
        require(response.header.requestId == decodedCount + 1, "stress response preserves request order");
        require(response.header.opcode == Opcode::PING && (response.header.flags & RESPONSE) != 0,
                "stress response opcode and direction");
        ++decodedCount;
    }
    require(decodedCount == expectedCount, "stress response count matches submitted requests");
}

}

/*
    功能：运行连接请求流水线压力测试并报告处理速率
    传参：无
    返回值：全部请求和响应正确时返回 0
*/
int main() {
    const auto startedAt = std::chrono::steady_clock::now();
    ByteBuffer input = makeStressInput(STRESS_REQUEST_COUNT);
    const std::size_t inputBytes = input.size();

    auto transport = std::make_unique<MemoryTransport>(std::move(input), 64U * 1024U, 64U * 1024U);
    MemoryTransport* transportView = transport.get();
    PendingExecutor executor;
    std::unique_ptr<Connection> connection;
    ConnectionCompletionPoster poster = [&connection](CommandResponse response) {
        connection->complete(std::move(response));
    };
    connection = std::make_unique<Connection>(77, 1, std::move(transport), 64, Endpoint{"127.0.0.1", 6380},
                                              DEFAULT_MAX_FRAME_BYTES, 1, {}, std::move(poster));

    connection->onReadable(executor, CONNECTION_READ_BUDGET);
    require(executor.submittedRequests() == 1, "stress begins with one in-flight request");

    for (std::size_t completedCount = 0; completedCount < STRESS_REQUEST_COUNT; ++completedCount) {
        executor.completePending();
        connection->onWritable(RESPONSE_WRITE_BUDGET);
    }

    require(executor.submittedRequests() == STRESS_REQUEST_COUNT, "all stress requests reach executor");
    require(!executor.hasPendingRequest(), "all stress completions are consumed");
    require(connection->getState() == ConnectionState::WAIT_HELLO,
            "unauthenticated ping stress keeps connection in negotiated start state");
    require(!transportView->writtenBytes().empty(), "stress connection produces response bytes");
    verifyStressResponses(transportView->writtenBytes(), STRESS_REQUEST_COUNT);

    const auto finishedAt = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = finishedAt - startedAt;
    const double requestsPerSecond = static_cast<double>(STRESS_REQUEST_COUNT) / elapsed.count();
    std::cout << "Network connection stress passed: " << STRESS_REQUEST_COUNT << " requests, " << inputBytes
              << " input bytes, " << transportView->writtenBytes().size() << " output bytes, "
              << elapsed.count() << " seconds, " << requestsPerSecond << " requests/second.\n";

    connection->close();
    return 0;
}
