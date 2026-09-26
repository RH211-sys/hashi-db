# 网络层

网络层实现位于本目录，职责从 TCP 字节流到结构化命令和响应：

```text
Network Controller -> NetworkServer -> Acceptor -> ReactorGroup
                                             Reactor -> Connection
                                                 FrameCodec -> CommandParser -> ICommandExecutor
```

## 当前实现范围

- 协议层：24 字节 MYDB 固定头、TLV 编解码、增量拆包/粘包和命令字段校验。
- Linux 传输：非阻塞 TCP 与 OpenSSL TLS、水平触发 epoll、eventfd 跨线程唤醒；每个 Connection 固定由一个 Reactor 管理。
- 连接处理：同一连接最多一个业务请求在途，后续完整请求进入有界流水线；完成结果回投原 Reactor，由所属事件循环更新连接和发送响应。
- 服务管理：TLS 默认必需，证书链和私钥在启动时装载并校验；明文仅在显式启用开发许可后开放，非回环监听还必须显式允许远程不安全连接。回环地址使用数字字面形式校验，不信任主机名。
- 停服语义：服务停止时停止接收新连接，立即关闭已有连接并丢弃尚未执行的 Reactor 任务，不执行停服排空。
- Windows 仅保留协议/接口可编译方向，网络服务端实现目标为 Linux。

## 背压与停止契约

1. 单连接流水线达到配置上限时暂停读取；已有请求按序完成、队列腾出容量后再恢复读取。
2. 每连接同一时刻最多一个业务请求在途，以保证同连接响应顺序。
3. Reactor 的通用任务、控制任务和完成结果队列均有容量上限；普通任务或控制任务队列满时拒绝新任务。
4. 完成结果队列满时放弃该结果并安排关闭对应连接，避免连接无限等待。
5. 服务统一采用立即停止：不等待业务排空或响应写完，连接和未执行任务由停服流程清理。

## 重要限制

1. v1 TLV `field_id` 数值见 `command/protocol_fields.h`；该表是网络字段编号契约，协议语义仍需随协议设计同步维护。
2. 当前没有真实认证提供者或 ACL；认证请求的校验应由执行层承担，连接会在认证成功响应后更新会话身份。`CLIENT_LIST`、`CLIENT_KILL` 虽可解析，但当前执行器不提供其业务处理；连接注册表接口尚未接入服务端装配。
3. 阶段性 `StorageControllerExecutor` 支持 PING/QUIT，并将 DELETE、PERSIST、FLUSH、REWRITE 接入存储 Controller；GET、ADD、UPDATE 因网络值类型 codec 未注册而返回 `TYPE_NOT_REGISTERED`。HELLO/AUTH 和未实现命令需由正式执行层补齐。
4. 存储 Controller 的 future 没有标准 C++20 continuation；桥接在独立、有界 worker 上等待，不得在 Reactor 线程等待。不可将存储层私有序列化格式直接暴露为网络值格式。
5. 网络目标独立于存储测试默认构建；纯协议/连接测试链接 `mydb_net_core`，服务端装配测试还需提供存储桥接依赖。

## 构建方向

网络层拆成四个目标，全部 `EXCLUDE_FROM_ALL`，不进入存储测试默认构建：

| 目标 | 内容 | 外部依赖 |
|---|---|---|
| `mydb_net_core` | 帧编解码、命令解析、会话、Connection、Reactor | 无（仅标准库） |
| `mydb_net_linux` | TCP transport、epoll poller、ReactorGroup、Acceptor、NetworkServer | 无（仅系统库） |
| `mydb_net_storage_bridge` | 阶段测试用存储桥接（`command_executor.cpp`、`controller/controller.cpp`） | 存储层头文件与 `third_party_lib/cereal_lib/include` |
| `mydb_net` | 聚合接口目标，链接 `mydb_net_linux` 与 `mydb_net_storage_bridge` | 继承上两行 |

存储桥接单独成目标，是为了让纯网络目标不被拖上存储层与 cereal 依赖；cereal 搜索路径只挂在桥接目标上（PRIVATE），不向消费方传播。

## 对接文档

- [网络层接口文档](../../doc/arch/net_doc/实现阶段/二轮开发/接口文档.md)：说明网络请求、执行器回调与结构化响应之间的契约。
