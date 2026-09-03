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

