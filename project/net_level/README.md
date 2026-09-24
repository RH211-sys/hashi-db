# 网络层

网络层实现位于本目录，职责从 TCP 字节流到结构化命令和响应：

```text
Network Controller -> NetworkServer -> Acceptor -> ReactorGroup
                                             Reactor -> Connection
                                                 FrameCodec -> CommandParser -> ICommandExecutor
```

## 当前实现范围

- 协议层：24 字节 MYDB 固定头、TLV 编解码、增量拆包/粘包和命令字段校验。
- Linux 传输：非阻塞 TCP、level-triggered epoll、eventfd 跨线程唤醒；Connection 固定由一个 Reactor 管理。
- 连接处理：每连接仅有一个业务请求在途，后续完整帧保留在有界输入缓冲中；异步 completion 回投 owner Reactor；关闭释放客户端容量槽。
- 服务管理：仅实现 Linux 本地明文 TCP；TLS 未实现且 `tlsRequired` 默认为 true，因此当前默认安全配置拒绝启动。开发时须同时显式设置 `tlsRequired=false` 与 `allowPlaintextForDevelopment=true`；非回环绑定还必须显式设置 `allowInsecureRemote=true`。回环仅接受数字字面地址 `127.0.0.1`、`::1`，不信任主机名。
- Windows 仅保留协议/接口可编译方向，网络服务端实现目标为 Linux。

## 背压与停止契约

1. 请求队列（每连接 pipeline）已满时，超出的后续请求直接丢弃；连接只暂停读取，不等待、不扩容。
2. 每连接同一时刻最多一个业务请求在途，以保证同连接响应顺序。
3. Reactor 的三类入队队列都有硬上限：通用任务、控制任务、完成结果。队列满时丢弃后续入队项。
4. 完成结果队列满时放弃该响应，并关闭对应连接；连接不会被留在“等待完成”的中间状态。
5. 优雅停止时不再接收新连接和新请求，但继续接收停止前已提交请求的完成结果，直到写完响应或超过排空时限；超时后强制关闭剩余连接。
6. 非优雅停止立即丢弃未执行任务并关闭全部连接。

## 重要限制

1. v1 TLV `field_id` 数值见 `command/protocol_fields.h`；此表是实现契约，尚待协议初审确认。响应共用该字段表中的 `STATUS_CODE` 和 `MESSAGE`。
2. 连接认证/ACL 目前没有认证提供者；客户端业务命令在认证前被拒绝。连接超时、全局在途背压、连接管理快照和 CLIENT_LIST/CLIENT_KILL 业务处理尚未实现（仅解析器识别相应 opcode/schema）。
3. 存储 Controller 的 future 没有标准 C++20 continuation；桥接必须在独立、有界 worker 上等待，不得在 Reactor 线程等待。若某 wire 类型没有已注册且往返一致的 codec，该类型读写应返回 `TYPE_NOT_REGISTERED`，不可直接把序列化私有格式暴露为协议。
4. 不得将 `project/net_level/*.cpp` 无条件链接进存储测试可执行文件；应建立独立网络 core/test CMake target。
5. `StorageControllerExecutor` 会引用存储层 `Controller` 符号；只使用协议/连接核心的网络测试应链接 `mydb_net_core`，涉及服务端装配的测试需要同时提供存储层编译产物。

## 构建方向

网络层拆成四个目标，全部 `EXCLUDE_FROM_ALL`，不进入存储测试默认构建：

| 目标 | 内容 | 外部依赖 |
|---|---|---|
| `mydb_net_core` | 帧编解码、命令解析、会话、Connection、Reactor | 无（仅标准库） |
| `mydb_net_linux` | TCP transport、epoll poller、ReactorGroup、Acceptor、NetworkServer | 无（仅系统库） |
| `mydb_net_storage_bridge` | 阶段测试用存储桥接（`command_executor.cpp`、`controller/controller.cpp`） | 存储层头文件与 `third_party_lib/cereal_lib/include` |
| `mydb_net` | 聚合接口目标，链接 `mydb_net_linux` 与 `mydb_net_storage_bridge` | 继承上两行 |

存储桥接单独成目标，是为了让纯网络目标不被拖上存储层与 cereal 依赖；cereal 搜索路径只挂在桥接目标上（PRIVATE），不向消费方传播。
