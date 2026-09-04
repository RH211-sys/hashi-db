```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 24473119 | QPS 815770
  latency(us): mean 9 | P50 8 | P99 45 | P999 156
  cache: hit 24473119 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 71263 | QPS 2375
  latency(us): mean 3369 | P50 21 | P99 98576 | P999 301751
  cache: hit 43456 / miss 6632 -> hit rate 86.7593%
  evict: 396 calls / 9859 items / avg 0.301568 ms per call / evict-ratio 0.39807%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [3] QPS - write-only (duration 30s) =====
  requests: 84010 | QPS 2800
  latency(us): mean 2858 | P50 134 | P99 26834 | P999 41958
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 420 calls / 11190 items / avg 0.0418595 ms per call / evict-ratio 0.0586033%
[prewarm hit-rate 2x] 1024 keys, 200 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 2x (total 200 MB, 200K each) (duration 30s) =====
  requests: 923323 | QPS 30777
  latency(us): mean 259 | P50 155 | P99 1007 | P999 1289
  cache: hit 727252 / miss 196071 -> hit rate 78.7646%
  evict: 3687 calls / 191724 items / avg 0.069597 ms per call / evict-ratio 0.855347%
[prewarm hit-rate 3x] 1536 keys, 300 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 3x (total 300 MB, 200K each) (duration 30s) =====
  requests: 596794 | QPS 19893
  latency(us): mean 402 | P50 290 | P99 1127 | P999 1455
  cache: hit 397065 / miss 199729 -> hit rate 66.533%
  evict: 3806 calls / 197912 items / avg 0.0676671 ms per call / evict-ratio 0.85847%
[prewarm hit-rate 4x] 2048 keys, 400 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 4x (total 400 MB, 200K each) (duration 30s) =====
  requests: 472907 | QPS 15763
  latency(us): mean 507 | P50 423 | P99 1212 | P999 1609
  cache: hit 267336 / miss 205571 -> hit rate 56.5304%
  evict: 3944 calls / 205088 items / avg 0.0686017 ms per call / evict-ratio 0.901883%
================== END, total 199s ==================

D:\Work\project\personal_project\C++\myDB\out\build\x64-Release\test\memo_test\CacheBench\memo_test2.exe (进程 26380)已退出，代码为 0 (0x0)。
要在调试停止时自动关闭控制台，请启用“工具”->“选项”->“调试”->“调试停止时自动关闭控制台”。
按任意键关闭此窗口. . 
```

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== [1] QPS - read-only (duration 30s) =====
  requests: 24086871 | QPS 802895
  latency(us): mean 9 | P50 8 | P99 42 | P999 142
  cache: hit 24086871 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 86003 | QPS 2866
  latency(us): mean 2792 | P50 21 | P99 89570 | P999 252837
  cache: hit 52845 / miss 7546 -> hit rate 87.5048%
  evict: 422 calls / 10960 items / avg 0.259213 ms per call / evict-ratio 0.364627%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== [3] QPS - write-only (duration 30s) =====
  requests: 101789 | QPS 3392
  latency(us): mean 2357 | P50 112 | P99 25217 | P999 40063
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 507 calls / 13073 items / avg 0.0405247 ms per call / evict-ratio 0.0684867%
[prewarm hit-rate 2x] 1024 keys, 200 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== hit-rate 2x (total 200 MB, 200K each) (duration 30s) =====
  requests: 945329 | QPS 31510
  latency(us): mean 253 | P50 151 | P99 987 | P999 1250
  cache: hit 744566 / miss 200763 -> hit rate 78.7626%
  evict: 3774 calls / 196248 items / avg 0.0715405 ms per call / evict-ratio 0.89998%
[prewarm hit-rate 3x] 1536 keys, 300 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== hit-rate 3x (total 300 MB, 200K each) (duration 30s) =====
  requests: 609983 | QPS 20332
  latency(us): mean 393 | P50 284 | P99 1108 | P999 1418
  cache: hit 405693 / miss 204290 -> hit rate 66.5089%
  evict: 3892 calls / 202384 items / avg 0.0721454 ms per call / evict-ratio 0.935967%
[prewarm hit-rate 4x] 2048 keys, 400 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== hit-rate 4x (total 400 MB, 200K each) (duration 30s) =====
  requests: 354255 | QPS 11808
  latency(us): mean 677 | P50 566 | P99 2564 | P999 3635
  cache: hit 200660 / miss 153595 -> hit rate 56.6428%
  evict: 2954 calls / 153608 items / avg 0.0917749 ms per call / evict-ratio 0.903677%
================== END, total 202s ==================
```

1. 数据大小均为200k，没有出现大小随机分布（原本应该在1k~10M之间随机分布/正态分布，而不是全是200k的大小）
2. **读命中用原子时间戳更新**，让时间维度生效
3. **读7写3 拆成"无注入"和"有注入"两版**，拆清瓶颈
4. **加一个 1K~10M 大小分布 + 纯读 + 冷热的命中率场景**
5. 注入 key 参与少量读，别纯污染





## 优化1

**修复写优先锁的防读饥饿失效**

- `writersWaiting` 从 bool 改成计数器，多写者排队时读者不会被误放
- `onReadDone` / `onWriteDone` 封装进 `unlock_shared` / `unlock` 内部，调用方不会漏调

**`modData` 旧实体锁外析构**

- 锁内 `std::move` 移出旧实体，解锁后自然析构，避免大对象释放拖长锁持有时间

**测试数据大小分布修正**

- 命中率档从固定 200K 改成对数正态 1K~10M，让大小维度在淘汰中真正生效

**写路径移动语义**

- 测试里 `modData` 传参用 `std::move(makeData(...))`，避免无谓拷贝

二次测试

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== [1] QPS - read-only (duration 30s) =====
  requests: 29327311 | QPS 977577
  latency(us): mean 8 | P50 5 | P99 40 | P999 149
  cache: hit 29327311 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 51146 | QPS 1704
  latency(us): mean 4698 | P50 22 | P99 147705 | P999 354730
  cache: hit 31208 / miss 4806 -> hit rate 86.6552%
  evict: 273 calls / 7264 items / avg 0.44122 ms per call / evict-ratio 0.40151%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== [3] QPS - write-only (duration 30s) =====
  requests: 104424 | QPS 3480
  latency(us): mean 2298 | P50 112 | P99 24181 | P999 39182
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 522 calls / 13259 items / avg 0.0412682 ms per call / evict-ratio 0.0718067%
[prewarm hit-rate 2x] 1015 keys, 200 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== hit-rate 2x (total 200 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 771750 | QPS 25725
  latency(us): mean 310 | P50 106 | P99 3225 | P999 5016
  cache: hit 612196 / miss 159554 -> hit rate 79.3257%
  evict: 2990 calls / 156002 items / avg 0.212546 ms per call / evict-ratio 2.11838%
[prewarm hit-rate 3x] 1560 keys, 300 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== hit-rate 3x (total 300 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 374677 | QPS 12489
  latency(us): mean 640 | P50 314 | P99 5006 | P999 12223
  cache: hit 254748 / miss 119929 -> hit rate 67.9914%
  evict: 2177 calls / 119177 items / avg 0.299603 ms per call / evict-ratio 2.17412%
[prewarm hit-rate 4x] 2084 keys, 400 MB, writing & flushing to disk...
[prewarm done] load running for 30s

===== hit-rate 4x (total 400 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 255532 | QPS 8517
  latency(us): mean 939 | P50 497 | P99 8067 | P999 18712
  cache: hit 144318 / miss 111214 -> hit rate 56.4775%
  evict: 2134 calls / 111689 items / avg 0.31929 ms per call / evict-ratio 2.27121%
================== END, total 201s ==================
```

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 16799606 | QPS 559986
  latency(us): mean 14 | P50 9 | P99 75 | P999 303
  cache: hit 16799606 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 34868 | QPS 1162
  latency(us): mean 6886 | P50 28 | P99 193502 | P999 427694
  cache: hit 21293 / miss 3331 -> hit rate 86.4725%
  evict: 190 calls / 5329 items / avg 0.567026 ms per call / evict-ratio 0.359117%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [3] QPS - write-only (duration 30s) =====
  requests: 67584 | QPS 2252
  latency(us): mean 3552 | P50 239 | P99 30723 | P999 48149
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 357 calls / 9339 items / avg 0.0544594 ms per call / evict-ratio 0.0648067%
================== END, total 91s ==================
```



- 修改读写锁参数
  `explicit WritePrefMutex(int batchSize = 8, int upDisEdge = 5, int minDisEdge = 0);	// batchSize：`
  `下界触发时放行的一批读数量`

```
================== START (performance benchmark) ==================
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 93263 | QPS 3108
  latency(us): mean 2574 | P50 19 | P99 62839 | P999 290607
  cache: hit 56908 / miss 8544 -> hit rate 86.9462%
  evict: 484 calls / 12310 items / avg 0.302994 ms per call / evict-ratio 0.48883%
================== END, total 30s ==================

================== START (performance benchmark) ==================
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 46287 | QPS 1542
  latency(us): mean 5197 | P50 25 | P99 151877 | P999 371530
  cache: hit 28422 / miss 4228 -> hit rate 87.0505%
  evict: 241 calls / 6464 items / avg 0.637896 ms per call / evict-ratio 0.512443%
================== END, total 30s ==================
```

优化异步后：

```
================== START (performance benchmark) ==================
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 37334 | QPS 1244
  latency(us): mean 6436 | P50 21 | P99 194500 | P999 431473
  cache: hit 22782 / miss 3573 -> hit rate 86.4428%
  evict: 206 calls / 5302 items / avg 0.547218 ms per call / evict-ratio 0.375757%
================== END, total 30s ==================
```

优化IO后（淘汰中批量写入）

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 16846539 | QPS 561551
  latency(us): mean 14 | P50 9 | P99 75 | P999 309
  cache: hit 16846539 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 537195 | QPS 17906
  latency(us): mean 446 | P50 11 | P99 15555 | P999 49264
  cache: hit 328192 / miss 48138 -> hit rate 87.2086%
  evict: 967 calls / 24745 items / avg 0.13455 ms per call / evict-ratio 0.4337%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [3] QPS - write-only (duration 30s) =====
  requests: 255417 | QPS 8513
  latency(us): mean 940 | P50 27 | P99 23549 | P999 56246
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 1241 calls / 31160 items / avg 0.0408719 ms per call / evict-ratio 0.169073%
================== END, total 91s ==================
```

删除剩余同步后：

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 28417564 | QPS 947252
  latency(us): mean 8 | P50 6 | P99 36 | P999 123
  cache: hit 28417564 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 628974 | QPS 20965
  latency(us): mean 381 | P50 10 | P99 14033 | P999 41415
  cache: hit 383432 / miss 57346 -> hit rate 86.9898%
  evict: 1135 calls / 29022 items / avg 0.0905868 ms per call / evict-ratio 0.34272%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [3] QPS - write-only (duration 30s) =====
  requests: 252674 | QPS 8422
  latency(us): mean 950 | P50 27 | P99 23733 | P999 54919
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 1246 calls / 31388 items / avg 0.043 ms per call / evict-ratio 0.178593%
[prewarm hit-rate 2x] 1015 keys, 200 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 2x (total 200 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 751545 | QPS 25051
  latency(us): mean 319 | P50 9 | P99 4036 | P999 6831
  cache: hit 595654 / miss 155891 -> hit rate 79.2573%
  evict: 2856 calls / 148581 items / avg 0.225016 ms per call / evict-ratio 2.14215%
[prewarm hit-rate 3x] 1560 keys, 300 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 3x (total 300 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 495728 | QPS 16524
  latency(us): mean 484 | P50 9 | P99 4270 | P999 13237
  cache: hit 334313 / miss 161415 -> hit rate 67.4388%
  evict: 2910 calls / 157908 items / avg 0.228588 ms per call / evict-ratio 2.2173%
[prewarm hit-rate 4x] 2084 keys, 400 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 4x (total 400 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 323535 | QPS 10784
  latency(us): mean 741 | P50 12 | P99 8417 | P999 11771
  cache: hit 182184 / miss 141351 -> hit rate 56.3104%
  evict: 2678 calls / 140424 items / avg 0.273609 ms per call / evict-ratio 2.44241%
================== END, total 182s ==================
```

读

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 22454742 | QPS 748491
  latency(us): mean 10 | P50 8 | P99 49 | P999 167
  cache: hit 22454742 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
================== END, total 30s ==================
```

