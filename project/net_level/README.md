# 服务端网络层架构骨架

本目录只描述服务端网络层的模块边界和对外接口，当前阶段不包含具体实现。

## 模块关系

```text
controller/Controller
        │  服务生命周期、配置、依赖组装
        ▼
server/NetworkServer ── acceptor/connection
        │
        ▼
reactor/Reactor ── transport/Transport
        │
        ▼
protocol/FrameCodec ── command/CommandParser
        │                         │
        └─────────────────────────▼
                    command/ICommandExecutor
                                  │
                    阶段测试：存储 Controller 桥接
```

## 当前阶段约束

1. 只建立头文件、类型和接口，不写 socket、epoll、TLS、线程和序列化的具体实现。
2. 网络层必须拥有命令解析模块，不能让网络层直接把原始字节交给存储 Controller。
3. `StorageControllerExecutor` 是临时阶段测试适配器；后续由正式命令执行层替换。
4. 对外开放的服务接口集中在 `controller/controller.h`，内部模块通过接口互相依赖。
5. 每个模块文件开头使用“模块名/功能描述”注释，每个函数使用统一的中文块注释模板。

## 后续实现顺序

协议类型与 FrameCodec → 命令解析 → fake transport/poller → 单 Reactor → 存储桥接 → Linux epoll/TLS。
