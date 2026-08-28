

# Hashi-DB

Hashi-DB 是一个基于 C++ 开发的高性能内存数据库系统，采用模块化设计，专注于提供高效的缓存管理和数据操作能力。

## 项目特点

- **类型安全的数据操作**：支持 INT、LONG、DOUBLE、STRING 和自定义类型的数据存储与操作
- **灵活的 CRUD 架构**：基于反射表和函数指针数组的轻量级操作层
- **内存管理优化**：智能内存分配策略与数据过期清理机制
- **命令式设计**：统一的缓存与磁盘 IO 命令处理框架
- **客户端-服务端架构**：支持网络通信的分布式部署

## 核心模块

```
hashi-db/
├── main.cpp                 # 程序入口
├── CMakeLists.txt           # 构建配置
└── project/
    └── data_memo_level/
        └── memory/
            └── data_opt/    # 数据操作层 (CRUD)
```

## 数据类型支持

| 类型标识 | 描述 |
|---------|------|
| `INT_OPT` | 整数类型 |
| `LONG_OPT` | 长整型 |
| `DOUBLE_OPT` | 双精度浮点 |
| `STRING_OPT` | 字符串类型 |
| `CUSTOM_OPT` | 自定义类型 |

## 操作接口

| 操作类型 | 功能说明 |
|---------|----------|
| `INSERT` | 新增数据 |
| `DELETE` | 删除数据 |
| `MODIFY` | 修改数据 |
| `SELECT` | 查询数据 |

## 构建指南

```bash
# 创建构建目录
mkdir build && cd build

# 生成 Makefile
cmake ..

# 编译项目
make
```

## 项目文档

详细设计文档位于 `doc/` 目录：

- **缓存设计**：`doc/plan/exec_level/cache_exe.md`
- **磁盘操作**：`doc/plan/exec_level/disk_exe.md`
- **内存管理**：`doc/plan/memo_manage_level/`
  - 分配策略：`memo_assign.md`
  - 清理策略：`memo_clean.md`
- **命令设计**：`doc/plan/command_map_level/`
- **网络通信**：`doc/plan/net_level/`（服务端/客户端）
- **日志系统**：`doc/plan/logs_level/log.md`

## 开发日志

项目开发进度记录在 `doc/log/` 目录，包含每日的开发目标、实现细节和问题记录。

## 许可证

本项目遵循开源协议，具体许可证信息请查阅项目仓库。