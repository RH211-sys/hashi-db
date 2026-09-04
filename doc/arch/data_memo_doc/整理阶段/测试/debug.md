```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 3214795 | QPS 107159
  latency(us): mean 74 | P50 63 | P99 267 | P999 448
  cache: hit 3214795 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 29166 | QPS 972
  latency(us): mean 8237 | P50 128 | P99 210437 | P999 481059
  cache: hit 17534 / miss 3036 -> hit rate 85.2406%
  evict: 167 calls / 4835 items / avg 2.37571 ms per call / evict-ratio 1.32248%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [3] QPS - write-only (duration 30s) =====
  requests: 26489 | QPS 882
  latency(us): mean 9061 | P50 4724 | P99 56746 | P999 91140
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 162 calls / 4354 items / avg 1.19228 ms per call / evict-ratio 0.64383%
[prewarm hit-rate 2x] 1024 keys, 200 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 2x (total 200 MB, 200K each) (duration 30s) =====
  requests: 304998 | QPS 10166
  latency(us): mean 786 | P50 478 | P99 3321 | P999 4354
  cache: hit 240138 / miss 64860 -> hit rate 78.7343%
  evict: 1227 calls / 63804 items / avg 2.04376 ms per call / evict-ratio 8.35896%
[prewarm hit-rate 3x] 1536 keys, 300 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 3x (total 300 MB, 200K each) (duration 30s) =====
  requests: 477366 | QPS 15912
  latency(us): mean 502 | P50 335 | P99 1948 | P999 2414
  cache: hit 317548 / miss 159818 -> hit rate 66.5209%
  evict: 3052 calls / 158704 items / avg 1.11703 ms per call / evict-ratio 11.3639%
[prewarm hit-rate 4x] 2048 keys, 400 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 4x (total 400 MB, 200K each) (duration 30s) =====
  requests: 362303 | QPS 12076
  latency(us): mean 662 | P50 532 | P99 2158 | P999 2928
  cache: hit 205551 / miss 156752 -> hit rate 56.7346%
  evict: 3015 calls / 156780 items / avg 1.13363 ms per call / evict-ratio 11.393%
================== END, total 204s ==================

D:\Work\project\personal_project\C++\myDB\out\build\x64-Debug\test\memo_test\CacheBench\memo_test2.exe (进程 23164)已退出，代码为 0 (0x0)。
要在调试停止时自动关闭控制台，请启用“工具”->“选项”->“调试”->“调试停止时自动关闭控制台”。
按任意键关闭此窗口. . 
```

删除同步后：

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 3842882 | QPS 128096
  latency(us): mean 62 | P50 54 | P99 176 | P999 249
  cache: hit 3842882 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 212248 | QPS 7074
  latency(us): mean 1130 | P50 51 | P99 29177 | P999 64735
  cache: hit 129534 / miss 19399 -> hit rate 86.9747%
  evict: 483 calls / 12814 items / avg 1.39784 ms per call / evict-ratio 2.25052%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [3] QPS - write-only (duration 30s) =====
  requests: 74978 | QPS 2499
  latency(us): mean 3205 | P50 311 | P99 54239 | P999 106537
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 353 calls / 9475 items / avg 1.20673 ms per call / evict-ratio 1.41992%
[prewarm hit-rate 2x] 1015 keys, 200 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 2x (total 200 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 274497 | QPS 9149
  latency(us): mean 873 | P50 31 | P99 10121 | P999 16819
  cache: hit 218269 / miss 56228 -> hit rate 79.516%
  evict: 1036 calls / 53806 items / avg 2.4389 ms per call / evict-ratio 8.42234%
[prewarm hit-rate 3x] 1560 keys, 300 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 3x (total 300 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 166429 | QPS 5547
  latency(us): mean 1442 | P50 38 | P99 11117 | P999 31906
  cache: hit 112006 / miss 54423 -> hit rate 67.2996%
  evict: 982 calls / 53923 items / avg 2.60142 ms per call / evict-ratio 8.51531%
[prewarm hit-rate 4x] 2084 keys, 400 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 4x (total 400 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 198381 | QPS 6612
  latency(us): mean 1209 | P50 43 | P99 11119 | P999 23319
  cache: hit 111338 / miss 87043 -> hit rate 56.1233%
  evict: 1658 calls / 87036 items / avg 1.79319 ms per call / evict-ratio 9.91037%
================== END, total 188s ==================
```

