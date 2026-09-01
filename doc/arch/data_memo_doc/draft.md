# MyDB 存储层回忆文档（2026-09-01，压缩上下文后先读此文件）

## 项目概况

- 个人 C++ 数据库项目：Windows + VS2022 + MSVC + CMake + C++20，所有文件 UTF-8
- 当前阶段：存储层（data_memo_level）功能完成，测试全部 PASS；下阶段按「待办」优化

## 代码结构

- `project/data_memo_level/`
  - `controller/` — Controller 统一入口（组装 Cache/Disk/DiskThread，同步返回 int）
  - `cache/` — Cache：缓存 + LRU 淘汰
  - `disk/` — Disk：持久化（追加写 + 两段式校验码）
  - `common/` — WritePrefMutex（写优先读写锁）
  - `controller/taskThreads.h` — ReadPool（读线程池，预留未用）/ WriteThread（预留未用）/ DiskThread（磁盘单线程，双队列差距调度，submit 返回 future）
  - `data_type.h` — Val 结构 + typeReg 类型注册表 + DEFINE_DATA_TYPE / THE_SIZE 宏
  - `protocol.h` — 错误码 + 磁盘协议常量
- `test/memo_test/test1/` — 功能测试工程（memo_test1，VS 可单独启动）
- main.cpp 已删除；根 CMakeLists 中 myDB 主程序目标已注释（测试期）

## 关键设计

### 错误码（protocol.h）
SUCCESS=200 / KEY_EXIST=201 / FIND_FAILED=202 / TYPE_VALID=203 / EXPIRED=204 / MEMO_OUT=300 / LONG_NAME=301 / UNKNOWN_ERROR=400 / FILE_OPEN_FILED=500

### 磁盘记录布局
`[校验码 1B][实体大小 4B][更新时间 8B(微秒)][过期时间 8B(微秒,永久写0)][是否永久 1B][类型名 32B][变量名 32B][实体 dataSize]`，头部固定 86B
- **追加写**：旧记录留空洞，inDisk 指向最新偏移，reWrite 回收空洞
- **两段式写**：先写 CHECK_BROKEN 整条，写完回写 CHECK_VALID（写一半崩溃 → 重建截断）
- 类型名必须落盘：std::any 类型擦除，读盘靠 typeName 查注册表反序列化

### 缓存与 LRU
- Cache：`unordered_map<string, Val> cache_db` + WritePrefMutex + LRU 哈希链表（哨兵 start/end，头=最近，尾=最久）
- Val：typeName / dataSize / isPermanent / expireTime / updateTime / isDirty / entity(std::any)
- **LRU_out**：目标 curSize < 0.9×memoSize；先删过期（缓存 + inDisk 索引，防复活），再尾部采样 15 个算分入大顶堆 `priority_queue<pair<double,string>>`
- **分数 = 时间档位×30 + ln(dataSize)**；五档 <1min/<1h/<1d/<7d/≥7d，档距 30 > ln(memoSize) 上限
- 脏数据淘汰：锁内 `val = std::move(it->second)` 移动出迭代器 → 删链表/缓存/curSize → 锁外提交 `disk->persisData(victim, val)`（带 Val 版，DISK_TASK，提交即返回不等待）
- 所有增长点（插入/回填/改大）锁外 `if (curSize > memoSize) LRU_out()`

### 接口语义（Controller 同步返回 int）
- **addData**：缓存或磁盘已有 → KEY_EXIST；**不主动刷盘**（落盘靠淘汰/flushDisk/persisAll）
- **selData**：命中缓存先检查过期（过期 → 删缓存+链表+curSize+提交磁盘删除任务 → EXPIRED）；未命中走磁盘，回填前锁外检查过期（过期 → 提交磁盘删除 → EXPIRED）；回填后超限触发 LRU_out
- **modData**：命中替换标脏；未命中查磁盘存在则回填标脏（不主动刷盘）
- **delData**：缓存命中删除即算成功，磁盘无记录（从未落盘）不算失败；缓存磁盘都没有 → FIND_FAILED
- **persisVar（无 Val 版）**：从缓存取数据，**缓存没有 → FIND_FAILED**（被淘汰的数据无法 persisVar）
- **persisAll**：收集缓存中未过期脏数据批量写；**flushDisk**：删缓存中过期数据 + 刷全部脏数据
- **reWrite**：整理所有数据到文件前部 + resize_file 截断回收空洞；原地重写，崩溃=数据全损交运维（概率极小，双文件方案大材小用）

### 设计结论（原 draft 摘要，勿推翻）
- isDirty 先清后写：写文件前锁内清脏，写失败置回，防写文件期间新修改被误清
- 删除 = 纯内存 inDisk.erase，不落文件标记；崩溃后已删数据复活可容忍
- MVCC 不采用（锁临界区微秒级，瓶颈在磁盘 IO；多版本内存翻倍）
- 不用字节缓冲方案（数据存两份内存翻倍）
- 批量持久化：一次文件开关写多条

### 线程
- DiskThread：单线程双队列（cacheTasks 快 / diskTasks 慢），差距调度（缓存优先，taskGap 到 upEdge=15 让位磁盘一个任务）；submit 返回 future，get() 阻塞；析构处理完队列后 join
- Cache 持 Disk/DiskThread 的 shared_ptr（注入），Controller 也持 shared_ptr 共享所有权

### 类型注册
- `DEFINE_DATA_TYPE(name)`：totalType.insert + typeReg.emplace（序列化/反序列化函数），**必须在函数作用域内调用**（全局作用域 MSVC 解析为声明报 C3927）
- 用户类型必须实现：静态 getClassName / 模板 serialize / 静态 theSize

## 测试

- 结构：根 CMakeLists `add_subdirectory(test)` → test/CMakeLists.txt → test/memo_test/CMakeLists.txt（自动收集子测试目录 + STORAGE_SOURCES 共享 project/*.cpp，排除 third_party）→ 子测试各自 add_executable
- test1 功能测试（全 PASS）：增/查/改/删/过期/LRU 淘汰回填/修改后淘汰落盘/persisVar/persisAll/flushDisk/reWrite
- 运行：VS 启动 memo_test1；断言输出 [PASS]/[FAIL] 并计数，失败返回码 1
- cout 输出用英文（VS 控制台编码问题）；数据文件 test1_data.dat 测试后删除
- 测试配置：缓存 64KB（触发 LRU），磁盘 64MB

## 本轮修复的 Bug（已修复）

1. **reWrite 死锁**：writeCacheData 缓存未命中分支直接 return 未 unlock_shared → 读锁泄漏，后续写锁永久等待 → 先解锁再返回
2. **delData 语义**：缓存命中删除成功，磁盘无记录不算失败
3. **selData 命中路径无过期检查**：命中已过期数据返回 SUCCESS → 加过期检查（删缓存+链表+curSize+提交磁盘删除+EXPIRED）

## 待办（下阶段，按用户要求）

1. **磁盘模块持久化代码组织**：封装序列化/反序列化接口，把 persisData 三个重载 + reWrite writeCacheData 重复的「检查→序列化→写记录→更新 inDisk/curSize」流程统一封装
2. **调用读线程池读缓存（lock_shared）**：ReadPool 已实现未启用，读任务走读线程池并发
3. **拷贝改移动，零拷贝**：C++ 语法特性优化热点路径（如 selData `res = it->second.entity`、Val 传递、批量 vector 收集）
4. **磁盘线程挂后台，controller 异步化**：调用立即返回，查返回码时再阻塞；该异步的异步，不要同步——controller 同步缓存后同步调用磁盘是性能问题，controller 应返回 future

## 未实现

- 启动时从数据文件扫描恢复 inDisk 索引（重建）：交给管理层，磁盘模块不做

## 用户工作偏好（重要）

- 只写用户要求的代码；不要动用户的注释
- 有问题先提问，不要猜测，不要乱想
- 回答简洁；讨论确认后再写代码
- 忽略 clangd 报错（用户用 MSVC；clangd 因 cereal include 路径未配置级联报错）
