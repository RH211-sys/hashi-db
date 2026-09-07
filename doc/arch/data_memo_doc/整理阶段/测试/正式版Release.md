# Release删除测试点代码

## 无预热数据（缓存充足）

首轮：

```
======== START (performance benchmark, write-inflight edition) ========
[prewarm [1] QPS - read-only] pool 344 keys, 101 MB, writing & flushing to disk...
[prewarm done] load running for 30s (WRITE_INFLIGHT=4)
[read-only] hot subset 344 keys / 101 MB (memory-hit reads; cold/disk reads -> hit-rate scenes)
===== [1] QPS - read-only (duration 30s) =====
QPS 650117 | P99 62 us | hit rate 100%

[prewarm [2] QPS - read70/write30] pool 344 keys, 101 MB, writing & flushing to disk...
[prewarm done] load running for 30s (WRITE_INFLIGHT=4)
===== [2] QPS - read70/write30 (duration 30s) =====
QPS 361925 | P99 643 us | hit rate 100%

[prewarm [3] QPS - write-only] pool 344 keys, 101 MB, writing & flushing to disk...
[prewarm done] load running for 30s (WRITE_INFLIGHT=4)
===== [3] QPS - write-only (duration 30s) =====
QPS 145117 | P99 705 us | hit rate 0%
======== END ========
```

![enough](image/cache_enough.jpg)

## 缓存爆满

缓存：数据量：磁盘 = 1:3:5

```
================== START (performance benchmark, write-inflight edition) ==================
[prewarm [1] QPS - read-only] pool 717 keys, 301 MB, writing & flushing to disk...
[prewarm done] load running for 30s (WRITE_INFLIGHT=4)
[read-only] hot subset 186 keys / 70 MB (memory-hit reads; cold/disk reads -> hit-rate scenes)
===== [1] QPS - read-only (duration 30s) =====
  QPS 643575 | P99 58 us | hit rate 99.9988%
[prewarm [2] QPS - read70/write30] pool 717 keys, 301 MB, writing & flushing to disk...
[prewarm done] load running for 30s (WRITE_INFLIGHT=4)
===== [2] QPS - read70/write30 (duration 30s) =====
  QPS 124780 | P99 3787 us | hit rate 96.3445%
[prewarm [3] QPS - write-only] pool 717 keys, 301 MB, writing & flushing to disk...
[prewarm done] load running for 30s (WRITE_INFLIGHT=4)
===== [3] QPS - write-only (duration 30s) =====
  QPS 49915 | P99 3863 us | hit rate 0%
```

![full](image/cache_full.jpg)

## 命中率测试

**纯读 + 冷热80/20 + 数据量2~3倍缓存。**

```
================== START (performance benchmark, write-inflight edition) ==================
[prewarm hit-rate 2x] 1015 keys, 200 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 2x (total 200 MB, log-normal 1K-10M) (duration 30s) =====
  QPS 48356 | P99 2954 us | hit rate 89.1665%
[prewarm hit-rate 3x] 1560 keys, 300 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== hit-rate 3x (total 300 MB, log-normal 1K-10M) (duration 30s) =====
  QPS 38428 | P99 2867 us | hit rate 84.1872%
================== END, total 60s ==================
```

![cache_hit](image/cache_hit.jpg)
