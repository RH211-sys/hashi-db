初步计划了数据库的分层结构及各层的功能

初步搭建 `数据存储层`的框架

完成`数据存储层`的`缓存`服务的CRUD基础功能

搭建项目框架：gitignore,CMakeLists等

## 存储结构大致内容

哈希

类型分为4个基本数据类型和自定义类型（模板）

4个基本类型

- INT
- LONG
- STRING
- DOUBLE

自定义类型通过**C++17新特性**的**any**接口实现

## 缓存结构设计

- **键值对**：键 = 变量名（string），值 = 类型 + 值实体
- **基本类型**：每种类型一个 `unordered_map`，直接存值实体，**无类型标记**（类型由 map 隐含）
- **自定义类型**：一个 `unordered_map`，值 = 类型标记 + 值实体（`std::any` 类型擦除）

```cpp
struct CustomValue {
    std::string type_name;   // 类型标记
    std::any entity;         // 值实体
};

class Memo {
    std::unordered_map<std::string, INT>    int_variable;
    std::unordered_map<std::string, LONG>   long_variable;
    std::unordered_map<std::string, DOUBLE> double_variable;
    std::unordered_map<std::string, STRING> string_variable;
    std::unordered_map<std::string, CustomValue> custom_variable;
};
```

## 静态逻辑组织（代码结构设计）

在CRUD 操作模块（DataOpt）中，写下相关的API服务，方便后续上层（执行层）的调用

- 由于缓存模块及其重要，需要尽可能的提高其性能，减少不必要的时间消耗
- 又为了简化代码，不做太多冗余的代码，故做如下的**改进和优化**

解决`CRUD中重复判断类型和变量名`的问题——一次查**反射表**同时得到**变量名是否存在**和**类型是否匹配**，结构如下

```
反射表：unordered_map<变量名，类型名>,都是string类型，用于直接判断变量的类型
函数地址数组：不同的数据类型需要调用不同的接口，这里用函数地址存放不同数据类型的处理方式
```

由于需要先判断变量存在性，再判断变量的数据类型，调用处理接口，再判断操作方式（如insert，这里正常思路使用0,1,2,3来代表不同的操作码，这里用宏封装）

**操作/协议宏**，用于封装**类型码**和**操作码**，正好，这里的**类型码**还能作为函数地址数组的下标

```
#define INT_OPT 0  
#define LONG_OPT 1 
#define DOUBLE_OPT 2  
#define STRING_OPT 3  
#define CUSTOM_OPT 4
#define INSERT 5   
#define DELETE 6   
#define MODIFY 7   
#define SELECT 8
```

## 缓存模块剩余的优化/处理（遗留）

- 自定义的类型可能需要提供回调函数，用于处理`查询`等操作
- 每个数据还需要封装**过期剩余时间**，缓存还需要**LRU**处理策略
- 协议宏需要单独放在某个文件
- 持久化策略（定时接口/手动接口）

## 当前代码结构

```
myDB/
├── CMakeLists.txt
├── main.cpp                        # 演示：CRUD 全流程 + 失败场景
├── data_memo/
│   └── memory/
│       ├── memo.h                  # 存储容器（Memo 类）
│       └── data_opt/
│           ├── data_opt.h          # DataOpt + 协议宏
│           └── data_opt.cpp        # baseOperation + 分发表实现
├── command_analysis/               # 命令分析层（空，待开发）
├── net/                            # 网络层（空，待开发）
└── doc/
    ├── plan/                       # 总体计划
    ├── data_memo_doc/              # 存储层设计文档
    └── log/                        # 开发日志（本文件）
```

还有`执行层`，`管理层`等层级，如果一切正常，开发完成后是7层结构

等缓存/数据存储层完成后，将进行大致的架构图的绘制