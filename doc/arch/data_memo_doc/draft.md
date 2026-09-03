- # 实现阶段：需求梳理与待办（草稿，已整理）

  > 本文件是本次大重构的决策存档与后续行动清单，据此继续开发，不再依赖对话上下文。
  > 状态标记：✅ 已拍板并已落地到代码 / 📋 已拍板待落地 / ⬜ 已确认但本次不做（后续项）

  ## 一、总体形态（✅ 已确认）

  - 存储引擎全部在后台线程执行，调用方**不阻塞**；所有公开接口（controller / cache / disk / 线程池）返回 `std::future`，上层需要结果时才 `.get()`。
  - 返回体：有实体数据的查询用 `std::future<SelResult>`，纯操作码的操作用 `std::future<int>`，一组查询将来可能 `std::future<std::vector<SelResult>>`。
  - 对应关系：
    - sel → `future<SelResult>`
    - add / del / mod → `future<int>`
  - **每个操作只有一条任务链、一个"主场线程"**，任务不嵌套、不混乱；绝对不允许"调用线程阻塞等待整条链"。

  ## 二、线程职责彻底分工（✅ 已确认）

  | 线程                          | 职责                                | 锁                         |
  | ----------------------------- | ----------------------------------- | -------------------------- |
  | 读线程池 ReadPool             | 只读缓存资源（命中查询）            | 共享锁 lock_shared，并发读 |
  | 写线程 WriteThread（单线程）  | 只写缓存资源（新增/修改/删除/回填） | 独占锁 lock，串行写        |
  | 磁盘线程 DiskThread（单线程） | 只操作磁盘索引 inDisk 与持久化文件  | 无锁（天然串行）           |

  - 磁盘线程内分两类任务，`taskGap` 让位调度：**CACHE_TASK（快，微秒级，访问 cache_db 索引）优先**；DISK_TASK（慢，毫秒级，文件 IO）在缓存任务堆积到上界（upEdge=15）时穿插执行，落到下界（lowEdge=-5）时重置。
  - 允许且仅允许两类"主场线程短暂等待磁盘线程"的跨池等待（用户拍板：可以保留，只要不乱）：
    1. 写线程上的任务（addData / modData / delData）短暂 `submit + get()` 等磁盘线程的 **CACHE_TASK**（微秒级 inDisk 查询），用于查重/确认存在；
    2. selData 未命中时，**读线程池任务**等待磁盘线程一次 **DISK_TASK**（读盘）。
  - 磁盘线程任务内发现问题**直接处理，不再二次提交**（例如读盘后发现已过期：磁盘线程内直接调 disk 删除，不再提交一个删除任务回去）。
  - 触发淘汰时 delData 自然阻塞：淘汰与写操作同在写线程串行队列里，排在前面的淘汰任务阻塞执行时，后面的 delData 等写线程即可（不需要额外处理）。

  ## 三、淘汰算法：删除 LRU 链表，改全局采样（✅ 已确认）

  ### 动机

  - 原 LRU 链表 + 命中刷新 updateTime：**每次读命中都要取读写锁写链表**，读路径被"必须写"卡住性能天花板。
  - 现改为：**命中路径纯读零写**——读命中只做共享锁 + 拷贝实体指针，不刷新链表、不刷新任何时间/顺序信息、不提交任何写任务。

  ### 决策点

  1. **删除 LRU 链表**：ListNode 结构、LRU_hash、start/end、LRU_addNode / LRU_moveToHead / LRU_removeNode 全部删除；函数改名 `LRU_out → evict`、`LRU_score → evictScore`。
  2. **updateTime 语义**：只在写操作/回填等写路径上更新，读命中不再刷新（放弃"越热 updateTime 越新"的时效性，接受一小部分命中率损失，换性能上限与复杂度下降）。
  3. **全局采样，不遍历整个缓存**：在 cache_db 内随机取样本（样本数沿用 15），算分公式不变（时间档位 × 档距 + ln(大小)，分数越高越优先淘汰）；分数基于"距上次**写入**的时间档位"。
  4. **优先队列 + 不每轮重采样**：采样算分后入大顶堆 `priority_queue`，每轮淘汰 pop 分数最高者；堆空才补采样。**锁粒度问题的解法**：不用跨轮大锁——每轮删除前在独占锁内 `find` 重新校验（是否已被删/是否过期/脏与干净以当时为准），校验通过才动手；干净的直接删，脏的锁内拷贝出 `shared_ptr`（轻量）后**锁外**提交 DISK_TASK persisData 刷盘，过期条目顺手删并批量提交 CACHE_TASK 删 inDisk；一轮结束后如容量仍不达标继续（不再重采样）。
  5. **淘汰在写线程内串行、阻塞执行**，循环直到 `curSize ≤ memoSize × 9/10` 才返回。
  6. 触发点：addData / modData / 回填路径在写线程内、锁外调用 evict（防刚插入就超限）。
  7. **竞态修正（用户明确要求）**：触发淘汰前比较 `curSize > memoSize` 时，**必须在解锁前先把 curSize 拷贝到局部变量，解锁后用拷贝值比较**（锁外读 curSize 有竞态；锁内拷贝开销极小）。

  ## 四、零拷贝：实体指针化（✅ 已确认）

  - `Val.entity` 与 `SelResult.entity` 都是 `std::shared_ptr<std::any>`。
  - 锁内**只复制指针，不复制实体**；写入/回填时实体 `std::make_shared` 移进堆，一次移动不拷贝。
  - 命中 / 回填 / 返回结果共享**同一实体指针**（上层只读约定，要修改请走 modData）。
  - 缓存删除条目只减引用计数，上层手里的指针不会悬空（shared_ptr 保活）。

  ## 五、接口流程细节（✅ 已确认，实现时逐条对照）

  ### addData（主场：写线程）→ future<int>

  1. **共享锁**（lock_shared）探测缓存：已存在 → 返回 KEY_EXIST；
  2. 向磁盘线程提交 `containsVar`（CACHE_TASK）并 `get()` 同步等待：磁盘已存在 → 返回 KEY_EXIST（不允许重复新增）；
  3. **锁外**构造 Val：`THE_SIZE` 在移动实体前计算；`v.entity = make_shared(move(entity))`；
  4. **独占锁**（lock）：锁内重新校验 `!contains` 后 `emplace(move(v))`，`curSize += dataSize`；锁内拷贝一份当前大小到局部变量再解锁；
  5. 解锁后若局部变量 > memoSize 触发 evict；返回 SUCCESS。

  ### modData（主场：写线程）→ future<int>

  1. **锁外**算好 `newSize = THE_SIZE(...)` 与 `newEntity = make_shared(move(entity))`（锁内只做记账与指针交换，临界区短）；
  2. **独占锁**内 find：命中 → `curSize = curSize - 旧size + newSize`、替换实体指针、updateTime = now、isDirty = true；锁内拷贝大小后解锁，超限触发 evict，返回 SUCCESS；
  3. 未命中 → 磁盘 `containsVar`（CACHE_TASK）等待：磁盘也没有 → 返回 FIND_FAILED；
  4. 磁盘存在 → 锁外构造 Val（isPermanent = true、isDirty = true、entity 用第 1 步同一实体指针）→ **独占锁**回填（锁内重校验防止并发插入），超限触发 evict。

  ### selData（主场：读线程池）→ future<SelResult>

  1. 共享锁查缓存：
     - **命中且未过期** → 拷贝实体指针（纯读零写，无任何写任务/刷新），返回 SUCCESS + 同一实体指针；
     - **命中但已过期** → 只提交清理任务不等待（两条**平级** push：写线程删缓存条目 + 磁盘线程 CACHE_TASK 删 inDisk），返回 EXPIRED；
  2. **未命中** → 提交磁盘线程任务（读池任务 `get()` 等待这一次读盘）：磁盘线程内完成"读盘 → 过期判定 → **过期直接删除**（已在磁盘线程，不二次提交）"；读出的 Val 回填**交写线程后台处理**（push 不等待；任务参数捕获含 shared_ptr 的 Val 保活实体，锁内重校验 contains 跳过则返回）；返回 SUCCESS + 同一实体指针；
  3. 磁盘也没有 → 返回 FIND_FAILED。

  ### delData（主场：写线程）→ future<int>

  - 独占锁删缓存条目 → 磁盘线程 CACHE_TASK 删 inDisk/文件 → 修正错误码返回。

  ### 磁盘线程主场操作

  - persisVar / persisAll / reWrite / flushDisk 等纯盘操作全部走 DiskThread，接口同步返回 future（当前已如此，保留）。

  ### Controller（上层）

  - 8 个 API 全部**透传** cache / diskThread 的 future，去掉现有同步 `.get()`；模板透传 addData / modData；头注释"CRUD 与持久化 API 全部同步返回错误码"及 readPool / writeThread 成员注释"暂未启用，预留"均已过时，随签名同步更新。

  ## 六、待办清单（按执行顺序）

  1. ✅ **data_type.h**：Val.entity 与 SelResult.entity → shared_ptr<std::any>（已完成）。
  2. ✅ **cache.h**：删除 ListNode / LRU_hash / start / end 与三个链表函数声明；evictScore / evict 命名与注释（全局采样语义，无任何 LRU 链表残留表述）；addData / modData 模板 → future<int>（共享锁探测 → 磁盘等待 → 独占锁插入，锁内快照 curSize 锁外比较）；selData → future<SelResult>（块注释说明命中零写 / 过期清理 / 磁盘线程读 + 过期直删 / 后台回填）；delData / persisVar×2 / reWrite 声明保持（delData 注释行保留原文"提交即返回，不阻塞；需要准确结果时对返回的 future 调用 get()"）；member 版 modData 重载声明保留。
  3. ⬜ **cache.cpp 重写**（下一步）：
     - evict 全局采样实现（随机桶/随机样本 15 → 算分 → 大顶堆 → 逐条独占锁 find 重校验删除；脏拷出锁外 DISK_TASK 刷盘；过期顺手删 + 批量 CACHE_TASK 删 inDisk；循环至 curSize ≤ memoSize×9/10；写线程内阻塞执行）；
     - selData 新实现（未来链：命中纯读 / 过期双平级 push / miss 磁盘任务内读 + 过期直删 / 回填 push 写线程）；
     - delData 整链写线程；
     - 删除全部 LRU 链表函数与构造函数里 start/end 初始化；
     - 命名 evict / evictScore，注释按"全局采样、无 LRU 链表"措辞。
  4. ⬜ **disk.cpp / disk.h**：接口签名零改动，适配 4 处序列化调用 `reg->second.first(val.entity)` → `reg->second.first(*val.entity)`（persisData ×2、批量 writeOne、reWrite writeCacheData）；disk 侧 selData 不填 val.entity，由缓存侧任务 make_shared 包装。
  5. ⬜ **controller.h / .cpp**：8 API 透传 future + 同步更新注释（含头注释与过时成员注释）。
  6. ⬜ 编译测试：测试模块调用点补 `.get()`，由用户编译并跑功能性测试。

  ## 七、本次不做（后续项，已确认以后处理）

  - **过期数据定时清理接口**：功能目前完全没有，接口形态待设计（上层定时调度）。
  - **flushDisk / reWrite 删过期条目与 curSize / 统计不同步 bug**：磁盘模块遍历删缓存过期条目时没有同步减 curSize，已知 bug，以后修。
  - **批量持久化**：缓存达到阈值后应持久化一批而非单个。
  - **member 版 modData**（`modData(varName, member, entity, resCode)`，用于改某个字段）未实现。
  - **磁盘模块代码优化**：序列化封装、代码组织、去冗余。
  - **原地重写**：磁盘重写直接在一个文件上原地覆盖（先读旧文件，顺序找正常数据，每次读一块内存大小，校验后从头顺序往下写覆盖），不用新文件；不用删除码是因为允许服务器异常重启/崩溃时回溯。
  - **文档整理**：修改算法设计的文档，分三个区块（最优 / 维度拓展 / 本次适应）并用 AI 重新比较。
  - **语法特性优化**：测试模块构建编写 + 用语法特性优化性能（零拷贝移动、返回指针等已部分落地，继续排查仍拷贝处）。
