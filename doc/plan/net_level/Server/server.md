# 服务端计划

## 服务端操作需求

1. 创建服务器
2. 通过输入命令的方式，进行数据库增删改查操作
3. 管理客户端（可以kick掉客户端）

## 服务端配置文件格式

```
memory/disk等空间单位：TB,GB,MB,KB,B
memo_to_disk_time:定时持久化时间

{
  "port": 6379,
  "memory": "32GB",
  "disk": "1TB"
  "tls": {
    "enabled": true,
    "cert_file": "/etc/mydb/cert.pem",
    "key_file": "/etc/mydb/key.pem"
  },
  "memo_to_disk_enabled": 0
  "memo_to_disk_time": 3600
  "max_client": 10000
}
```

使用json/yaml文件配置
