# Release删除测试点代码

## 无预热数据（缓存充足）

首轮：

```
================== START (performance benchmark) ==================
[prewarm [1] QPS - read-only] pool 2 keys, 1 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [1] QPS - read-only (duration 30s) =====
  requests: 24654850 | QPS 821828
  latency(us): mean 9 | P50 8 | P99 39 | P999 131
[prewarm [2] QPS - read70/write30] pool 2 keys, 1 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [2] QPS - read80/write20 (duration 30s) =====
  requests: 3482876 | QPS 116095
  latency(us): mean 68 | P50 9 | P99 1983 | P999 4760
[prewarm [3] QPS - write-only] pool 2 keys, 1 MB, writing & flushing to disk...
[prewarm done] load running for 30s
===== [3] QPS - write-only (duration 30s) =====
  requests: 950809 | QPS 31693
  latency(us): mean 252 | P50 16 | P99 4387 | P999 19967
```

