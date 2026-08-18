# Day1 开发日志（2026-08-18）

## 今日目标

搭建项目骨架 + 完成数据存储层·缓存方向的 CRUD 基础功能

## 一、环境搭建

| 项目 | 内容 |
|------|------|
| 平台 | Windows 11 + VS2022（调试用）/ Linux（服务端目标平台） |
| 构建 | CMake（Ninja x64-Debug），构建目录 `out/build` |
| 语言标准 | C++17 |

### CMakeLists.txt 重写

- 项目名改为 `myDB`，C++17
- `GLOB_RECURSE` 自动收集 `main.cpp` + 三个模块目录（command_analysis / data_memo / net）下的 `.cpp`，新增文件无需改 CMake
- 各模块目录加入 include 路径
- 警告级别：MSVC `/W4`，GCC/Clang `-Wall -Wextra`
- **MSVC 加 `/utf-8`**：解决中文注释编码问题（见下方"踩坑"）

### .gitignore

- 忽略 `out/`、`.vs/`、编译产物、`.vscode/*`（保留 settings/tasks）

## 二、存储结构设计（缓存方向）

设计定稿（参考 `doc/data_memo_doc/` 需求与可行性分析）：

- **键值对**：键 = 变量名（string），值 = 类型 + 值实体
- **基本类型**：每种类型一个 `unordered_map`，直接存值实体，**无类型标记**（类型由 map 隐含）
- **自定义类型**：一个 `unordered_map`，值 = 类型标记 + 值实体（`std::any` 类型擦除）
- 数据库**只存数据不存方法**

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

## 三、CRUD 操作层（DataOpt）

### 核心设计：反射表 + 函数指针数组

解决"判断两遍类型"的问题——一次查反射表同时得到**变量名是否存在**和**类型是否匹配**，类型分发只做一次：

```
baseOperation(type, name, op, val, res)
  ├─ typeToIndex(type)          ← 类型名 → 索引，只判断这一次
  ├─ 查反射表 reflect_[name]     ← 存在？类型匹配？(INSERT 时允许新增)
  ├─ opFuncs_[idx](...)         ← 按索引取函数地址，只分发这一次
  └─ DELETE 成功 → 清理反射记录
```

- **反射表** `reflect_`：`变量名 → 类型索引`（索引 0=int 1=long 2=double 3=string 4=自定义）
- **函数地址数组** `opFuncs_[5]`：成员函数指针，索引对应类型索引
- 自定义类型额外校验 `type_name` 一致（不同自定义类型索引都是 4）
- 查询结果用 `std::string*` 返回（为协议层铺路）；double 用 ostringstream 避免 `3.140000`

### 协议宏

```cpp
#define INT_OPT 0  #define LONG_OPT 1  #define DOUBLE_OPT 2  #define STRING_OPT 3  #define CUSTOM_OPT 4
#define INSERT 5   #define DELETE 6   #define MODIFY 7   #define SELECT 8
```

### 接口

```cpp
// type: "int"/"long"/"double"/"string"/自定义类型名
// op: INSERT / DELETE / MODIFY / SELECT
// val: 插入/修改传值，删除/查询传空 any；res: 查询结果，其余传 nullptr
bool baseOperation(const std::string& type, const std::string& name, int op,
                   std::any val, std::string* res);
```

## 四、今日踩坑记录

1. **CMakeCache 路径错位**：项目文件夹从 `D:\Work\personal_project\...` 移到 `D:\Work\project\personal_project\...`，旧缓存记录旧路径导致 CMake 拒绝生成 → **删除 `out/` 目录重新生成**。教训：移动项目文件夹后必须删构建缓存。
2. **中文注释编码（C4819）**：VS 默认按 GBK(936) 解析源文件，UTF-8 中文注释字节被误读导致语法解析崩坏、报错位置奇怪（C1075 花括号不匹配、main.cpp 出现 `->`、`std::cout` 报 using 声明错误）→ **CMake 加 `/utf-8`**。教训：报错位置奇怪且带 C4819 警告时，先怀疑编码。
3. **CMakeLists 手写源文件路径拼错**：`dataopt/`、`data_ot.cpp` 与实际不符 → GLOB 已自动收集，手动追加多余且错误，已删除。
4. **SELECT 分支误写 erase**：customOp 查询分支误写成 `*res = custom_variable.erase(name)`——查询是只读操作却删除了数据，且 `size_t` 赋给 `string` 编译不过。已修复。教训：SELECT 绝不能动数据。
5. **宏拼写**：`CUSTON_OPT` → `CUSTOM_OPT`。

## 五、遗留 TODO

- [ ] 自定义类型序列化（SELECT 目前返回类型名占位，协议层/磁盘层需要时实现）
- [ ] 过期策略 / LRU / 内存超阈值清理（memo_manage_level）
- [ ] 协议宏抽到独立 `protocol.h`（command_analysis 层需要引用）
- [ ] 磁盘层持久化（先写缓存，定时/手动/事件触发落盘）
- [ ] 命令分析层 → 网络层（Linux + epoll + TLS + 认证权限）

## 六、当前代码结构

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
