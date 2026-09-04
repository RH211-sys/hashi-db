最新测试：
第一轮：

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 28678175 | QPS 955939
  latency(us): mean 8 | P50 6 | P99 35 | P999 127
  cache: hit 28678175 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 817818 | QPS 27260
  latency(us): mean 293 | P50 10 | P99 5536 | P999 38674
  cache: hit 531460 / miss 41500 -> hit rate 92.7569%
  evict: 1062 calls / 22209 items / avg 0.159251 ms per call / evict-ratio 0.56375%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [3] QPS - write-only (duration 30s) =====
  requests: 115150 | QPS 3838
  latency(us): mean 2086 | P50 35 | P99 48624 | P999 96536
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 1430 calls / 16343 items / avg 0.0603783 ms per call / evict-ratio 0.287803%
[prewarm hit-rate 2x] 1015 keys, 200 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 2x (total 200 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 1212222 | QPS 40407
  latency(us): mean 197 | P50 8 | P99 3633 | P999 6083
  cache: hit 1080176 / miss 132046 -> hit rate 89.1071%
  evict: 2965 calls / 130372 items / avg 0.237229 ms per call / evict-ratio 2.34461%
[prewarm hit-rate 3x] 1560 keys, 300 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 3x (total 300 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 951648 | QPS 31721
  latency(us): mean 252 | P50 9 | P99 3599 | P999 12570
  cache: hit 802208 / miss 149440 -> hit rate 84.2967%
  evict: 3066 calls / 149278 items / avg 0.23352 ms per call / evict-ratio 2.38657%
[prewarm hit-rate 4x] 2084 keys, 400 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 4x (total 400 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 499103 | QPS 16636
  latency(us): mean 480 | P50 9 | P99 8882 | P999 12760
  cache: hit 393017 / miss 106086 -> hit rate 78.7447%
  evict: 2524 calls / 106497 items / avg 0.304932 ms per call / evict-ratio 2.56549%
================== END, total 182s ==================
```

第二轮

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 29055627 | QPS 968520
  latency(us): mean 8 | P50 6 | P99 34 | P999 128
  cache: hit 29055627 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 836222 | QPS 27874
  latency(us): mean 287 | P50 10 | P99 5039 | P999 39650
  cache: hit 542816 / miss 43079 -> hit rate 92.6473%
  evict: 1066 calls / 22789 items / avg 0.154991 ms per call / evict-ratio 0.550733%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [3] QPS - write-only (duration 30s) =====
  requests: 114591 | QPS 3819
  latency(us): mean 2096 | P50 35 | P99 48217 | P999 93062
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 1469 calls / 17470 items / avg 0.0579517 ms per call / evict-ratio 0.28377%
[prewarm hit-rate 2x] 1015 keys, 200 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 2x (total 200 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 1202598 | QPS 40086
  latency(us): mean 199 | P50 8 | P99 3693 | P999 6260
  cache: hit 1071522 / miss 131076 -> hit rate 89.1006%
  evict: 2961 calls / 129551 items / avg 0.234317 ms per call / evict-ratio 2.31271%
[prewarm hit-rate 3x] 1560 keys, 300 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 3x (total 300 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 623322 | QPS 20777
  latency(us): mean 385 | P50 10 | P99 5558 | P999 13620
  cache: hit 527406 / miss 95916 -> hit rate 84.6121%
  evict: 2019 calls / 96152 items / avg 0.333107 ms per call / evict-ratio 2.24181%
[prewarm hit-rate 4x] 2084 keys, 400 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 4x (total 400 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 451886 | QPS 15062
  latency(us): mean 531 | P50 9 | P99 8706 | P999 16618
  cache: hit 354486 / miss 97400 -> hit rate 78.4459%
  evict: 2293 calls / 97950 items / avg 0.349546 ms per call / evict-ratio 2.67169%
================== END, total 183s ==================
```

第三轮

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 29050266 | QPS 968342
  latency(us): mean 8 | P50 6 | P99 34 | P999 126
  cache: hit 29050266 / miss 0 -> hit rate 100%
  evict: 0 calls / 0 items / avg 0 ms per call / evict-ratio 0%
[prewarm [2] QPS - read70/write30] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read70/write30 (duration 30s) =====
  requests: 831308 | QPS 27710
  latency(us): mean 288 | P50 10 | P99 5256 | P999 38496
  cache: hit 540535 / miss 41932 -> hit rate 92.801%
  evict: 1071 calls / 22497 items / avg 0.158085 ms per call / evict-ratio 0.564363%
[prewarm [3] QPS - write-only] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [3] QPS - write-only (duration 30s) =====
  requests: 113870 | QPS 3795
  latency(us): mean 2108 | P50 34 | P99 47938 | P999 88841
  cache: hit 0 / miss 0 -> hit rate 0%
  evict: 1452 calls / 17094 items / avg 0.0601687 ms per call / evict-ratio 0.291217%
[prewarm hit-rate 2x] 1015 keys, 200 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 2x (total 200 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 1213990 | QPS 40466
  latency(us): mean 197 | P50 9 | P99 3651 | P999 6145
  cache: hit 1082986 / miss 131004 -> hit rate 89.2088%
  evict: 2979 calls / 129391 items / avg 0.232345 ms per call / evict-ratio 2.30719%
[prewarm hit-rate 3x] 1560 keys, 300 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 3x (total 300 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 956270 | QPS 31875
  latency(us): mean 250 | P50 9 | P99 3646 | P999 12636
  cache: hit 806730 / miss 149540 -> hit rate 84.3622%
  evict: 3070 calls / 149478 items / avg 0.228177 ms per call / evict-ratio 2.33501%
[prewarm hit-rate 4x] 2084 keys, 400 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 4x (total 400 MB, log-normal 1K-10M) (duration 30s) =====
  requests: 496259 | QPS 16541
  latency(us): mean 483 | P50 9 | P99 8831 | P999 12875
  cache: hit 390380 / miss 105879 -> hit rate 78.6646%
  evict: 2541 calls / 106370 items / avg 0.297564 ms per call / evict-ratio 2.52036%
================== END, total 182s ==================
```

## 读8写2业务

```
================== START (performance benchmark) ==================
[prewarm [2] QPS - read80/write20] pool 338 keys, 95 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read80/write20 (duration 30s) =====
  requests: 1065308 | QPS 35510
  latency(us): mean 225 | P50 8 | P99 4091 | P999 35585
  cache: hit 789920 / miss 62920 -> hit rate 92.6223%
  evict: 1060 calls / 21667 items / avg 0.21232 ms per call / evict-ratio 0.750197%
================== END, total 30s ==================
```

