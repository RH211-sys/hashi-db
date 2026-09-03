# 实现阶段：需求梳理与待办（草稿，已整理）

> 本文件是本次大重构的决策存档与后续行动清单，据此继续开发，不再依赖对话上下文。
> 状态标记：✅ 已拍板并已落地到代码 / ⬜ 待做或本次不做（后续项）

## 一、总体形态（✅ 已落地）

- 存储引擎全部在后台线程执行，调用方**不阻塞**；所有公开接口（controller / cache / disk / 线程池）返回 `std::future`，上层需要结果时才 `.get()`。
- 返回体：有实体数据的查询用 `std::future<SelResult>`，纯操作码的操作用 `std::future<int>`。
  - sel → `future<SelResult>`（含错误码 + 实体指针）
  - add / del / mod / persis / flush / reWrite → `future<int>`
- **每个操作只有一条任务链、一个"主场线程"**，任务不嵌套、不混乱；调用线程绝不阻塞等待整条链。

## 二、线程职责彻底分工（✅ 已落地）

| 线程 | 职责 | 锁 |
|---|---|---|
| 读线程池 ReadPool | 只读缓存资源（命中查询） | 共享锁 lock_shared，并发读 |
| 写线程 WriteThread（单线程） | 只写缓存资源（新增/修改/删除/回填） | 独占锁 lock，串行写 |
| 磁盘线程 DiskThread（单线程） | 只操作磁盘索引 inDisk 与持久化文件 | 无锁（天然串行） |

- 磁盘线程内分两类任务，`taskGap` 让位调度：**CACHE_TASK（快，微秒级，访问 cache_db 索引）优先**；DISK_TASK（慢，毫秒级，文件 IO）在缓存任务堆积到上界（upEdge=15）时穿插执行，落到下界（lowEdge=-5）时重置。
- 允许且仅允许两类跨池等待（用户拍板：可以保留，只要不乱）：
  1. **写线程任务**（addData / modData / delData）短暂 `submit + get()` 等磁盘线程 **CACHE_TASK**（微秒级 inDisk 查询），用于查重/确认存在；
  2. selData 未命中时，**读线程池任务**等待磁盘线程一次 **DISK_TASK**（读盘，毫秒级磁盘 IO）。
- 磁盘线程任务内发现问题**直接处理，不再二次提交**（如读盘后发现已过期：磁盘线程内直接调 disk 删除）。
- 淘汰阻塞进行时 delData 在写线程队列内自然等待，无需额外处理。

## 三、淘汰算法：删除 LRU 链表，改全局采样（✅ 已落地）

### 动机
- 原 LRU 链表 + 命中刷新 updateTime：每次读命中都要取读写锁写链表，读路径被"必须写"卡住性能天花板。
- 现改为**命中路径纯读零写**：读命中只做共享锁 + 拷贝实体指针，不刷新时间/顺序、不提交任何写任务。

### 决策点
1. **删除 LRU 链表**：ListNode 结构、LRU_hash、start/end、LRU_addNode / LRU_moveToHead / LRU_removeNode 全部删除；函数改名 `LRU_out → evict`、`LRU_score → evictScore`。
2. **updateTime 语义**：只在写操作/回填等写路径更新，读命中不刷新（接受一小部分命中率损失，换性能上限与复杂度下降）。
3. **全局采样，不遍历整个缓存**：从 cache_db 随机起点桶环形扫桶取样本（样本数 15），算分公式不变（时间档位 × 档距 + ln(大小)，分数越高越优先淘汰），分数基于"距上次**写入**的时间档位"。
4. **优先队列 + 不每轮重采样**：算分入大顶堆，每轮 pop 堆顶，堆空才补采。不用跨轮大锁——每轮删除前在**独占锁内 find 重新校验**（以当时为准）；干净的直接删，脏的锁内浅拷贝 Val（shared_ptr）后锁外 push DISK_TASK 刷盘。
5. **淘汰在写线程内串行、阻塞执行**，循环直到 `curSize ≤ memoSize × 9/10` 才返回；先批量删过期数据（独占锁内一次全遍历，只发生在写线程淘汰路径，不在读路径），inDisk 一次批量删（CACHE_TASK 等待完成防复活）。
6. 触发点：addData / modData / 回填路径在写线程内、锁外调用 evict（防刚插入就超限）。
7. **竞态修正（用户明确要求）**：触发淘汰前比较 `curSize > memoSize` 时，**先在锁内把 curSize 拷贝到局部变量，解锁后用拷贝值比较**（锁外读 curSize 有竞态；锁内拷贝开销极小）。evict 内部循环条件同样锁内快照。

## 四、零拷贝：实体指针化（✅ 已落地）

- `Val.entity` 与 `SelResult.entity` 都是 `std::shared_ptr<std::any>`。
- 锁内只复制指针，不复制实体；写入/回填时实体 `make_shared` + 移动进堆（值捕获 lambda 加 `mutable`，否则 move 退化为拷贝——已踩坑修复）。
- 命中 / 回填 / 返回结果共享同一实体指针（上层只读约定，修改请走 modData）。
- 缓存删除条目只减引用计数，上层手里的指针不悬空（shared_ptr 保活）。

## 五、接口流程细节（✅ 已落地，实现对照）

### addData（主场：写线程）→ future<int>
共享锁探测缓存（有 → KEY_EXIST）→ 磁盘 `containsVar` CACHE_TASK 等待（有 → KEY_EXIST）→ 锁外构造 Val（THE_SIZE 在移动前算）→ 独占锁内重校验后 emplace，锁内快照大小 → 解锁后超限 evict → SUCCESS。

### modData（主场：写线程）→ future<int>
锁外算 newSize + make_shared 新实体 → 独占锁 find 命中替换指针/记账/updateTime=now/isDirty=true（快照后解锁，超限 evict）→ 未命中查磁盘 containsVar（无 → FIND_FAILED）→ 有则锁外构造 Val（isPermanent=true、isDirty=true、同一实体指针）独占锁回填（超限 evict）。

### selData（主场：读线程池）→ future<SelResult>
共享锁查缓存：命中未过期 → 拷贝实体指针返回 SUCCESS（纯读零写）；命中已过期 → 两条平级 push（写线程删缓存 + 磁盘线程 CACHE_TASK 删 inDisk）后返回 EXPIRED；未命中 → 磁盘线程任务内完成"读盘 + 过期判定 + **过期直删**"，读池任务等待这次磁盘 IO；未过期的回填 push 写线程后台（mutable 值捕获 Val 保活实体，锁内重校验跳过），返回 SUCCESS + 同一实体指针；磁盘也没有 → FIND_FAILED。

### delData（主场：写线程）→ future<int>
整条链封装为单个写线程任务：独占锁删缓存条目 → CACHE_TASK 等磁盘删 inDisk → 修正码（缓存命中但磁盘无记录按 SUCCESS）。

### 磁盘线程主场操作
persisVar / persisAll / reWrite / flushDisk 全走 DiskThread（controller 内 flushDisk 直接 diskThread submit）。

### Controller（上层）
8 个 API 全部透传 future 并同步更新注释（头注释、成员注释、构造注释不再有"同步返回/LRU/预留"过时表述）。

## 六、已落地代码改动清单（本次重构）

1. ✅ **data_type.h**：Val.entity 与 SelResult.entity → shared_ptr<std::any>；注释同步。
2. ✅ **cache.h**：删 ListNode/LRU_hash/start/end 及链表函数声明；evictScore/evict 命名与全局采样注释；addData/modData 模板 future<int> + mutable 值捕获（共享锁探测 → 磁盘等待 → 独占锁插入 + 锁内快照）；selData → future<SelResult>；delData/persisVar×2/reWrite 声明；member 版 modData 重载声明保留。
3. ✅ **cache.cpp**：构造函数去链表初始化；evictScore/evict 全局采样实现（含批量删过期 + 批量删 inDisk 等待、随机桶采样、优先队列、锁内重校验、脏数据锁外刷盘）；selData 新实现（命中零写 / 过期双平级 push / miss 磁盘任务读 + 过期直删 / 回填 mutable push）；delData 整链写线程任务；bind/persisVar×2/reWrite 保留。
4. ✅ **disk.cpp**：4 处序列化调用 `reg->second.first(val.entity)` → `*val.entity`（persisData ×2、批量 writeOne、reWrite writeCacheData），接口签名零改动。
5. ✅ **controller.h/.cpp**：全 API 返回 future 透传（模板 addData/modData、selData → future<SelResult>、flushDisk 去 .get()），注释同步更新。
6. ✅ **test1.cpp**：按新接口重写（全调用 .get()；selData 取 SelResult 返回体，实体 `any_cast<T>(*entity)` 还原；注释更新为全局采样语义；第 8 节 persisVar 前 modData 回填保证 key 在缓存）。
7. ✅ **编译期修复**：值捕获 lambda 内移动/修改需 `mutable`（cache.h addData/modData、cache.cpp selData 回填），否则 const 成员使 move 退化为拷贝甚至编译错误。

## 七、测试阶段（当前）

### 目标
功能测试通过 = 新架构（异步 future / 线程分工 / 全局采样淘汰 / 实体指针零拷贝）在单机功能层面正确。

### 步骤
1. 全量重新生成 memo_test1（配置 x64-Debug），确认零错误（C4458 警告：bind 参数与成员同名，旧代码遗留的 /W4 警告，不阻塞）。
2. 运行 memo_test1，看输出：
   - 每个断言行 [PASS]/[FAIL]；
   - 结尾 `total N asserts, failed M`，M=0 为通过，退出码 0。
3. 断言覆盖点：
   - INSERT：新增成功 / 缓存与磁盘双重查重（重复新增 KEY_EXIST）；
   - SELECT：命中值与类型还原正确 / 不存在 FIND_FAILED；
   - MODIFY：命中替换值正确 / 不存在 FIND_FAILED；
   - DELETE：删除 SUCCESS / 二次删除 FIND_FAILED / 删后查 FIND_FAILED；
   - EXPIRED：过期命中返回 EXPIRED → 清理任务执行后（500ms）再查 FIND_FAILED；
   - EVICT：3000 条挤满 64KB 缓存触发多轮全局采样淘汰 → flushDisk 排空磁盘队列 → 抽检 key 从磁盘回填且值正确；
   - MODIFY+EVICT：脏数据淘汰落盘后回填拿到新值；
   - PERSIST：persisVar / persisAll / flushDisk / reWrite 全 SUCCESS，重写后查值正确、文件非空。

### 测试阶段的风险观察点（断言之外的时序）
- 过期清理是异步 push（写线程 + 磁盘线程），EXPIRED 后二次查询依赖 sleep 放行，时间敏感；
- 全局采样淘汰有随机性：抽检 key 可能在缓存中也可能已被挤出回填，值断言两种结果都覆盖；
- addData 全 .get() 使插入串行化，功能正确性优先，压力/并发另行测试；
- evict 刷盘是 fire-and-forget（DISK_TASK），落盘顺序靠 flushDisk().get() 保证。

### 测试阶段后续（本次之后）
- main.cpp（根目录演示程序）仍是旧同步签名，需适配 future 或废弃；
- 并发读写压力测试、淘汰压力、进程重启后从磁盘恢复（重写/回溯）验证；
- 多线程死锁/挂起观察（无日志机制，可临时加打印定位）。

## 八、已知问题与后续项（⬜ 本次不做）

- **flushDisk / reWrite 删缓存过期条目与 curSize 统计不同步 bug**：磁盘模块遍历删缓存过期条目时没有同步减 curSize，已知 bug，以后修。
- **过期数据定时清理接口**：目前没有定时清理功能，接口形态待设计（上层定时调度）。
- **批量持久化**：缓存达到阈值后应持久化一批而非单个（现有 persisData 批量版已供 flushDisk 用，阈值触发的批量持久化策略未做）。
- **member 版 modData**（`modData(varName, member, entity, resCode)`，改某字段）未实现。
- **磁盘模块代码优化**：序列化封装、代码组织、去冗余（disk.cpp 大量重复的记录读写逻辑）。
- **原地重写**：单文件原地覆盖重写方案（磁盘阈值利用率翻倍）；不用删除码是因为允许服务器异常重启/崩溃时回溯。
- **文档整理**：修改算法设计文档，分三个区块（最优 / 维度拓展 / 本次适应）并用 AI 重新比较。
- **语法特性/性能优化**：继续排查仍拷贝处（值传递的 string、entity 等），优化测试模块。
