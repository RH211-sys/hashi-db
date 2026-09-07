#pragma once
#ifndef _DISK_H_
#define _DISK_H_

#include "../data_type.h"
#include <atomic>
#include <fstream>
#include <set>
#include <utility>
#include <vector>
#include <list>
#include <unordered_map>

class Cache;

/*
	磁盘 IO 阶段统计（供性能归因，非功能）：序列化 / 文件写 / flush / 磁盘读的耗时与量
	计数为 relaxed 原子累加（磁盘线程与测试线程读，不做强一致）
*/
struct DiskIoStat {
	long long writeCnt = 0;		// 追加写记录数（appendRecord 成功）
	long long writeBytes = 0;	// 追加写总字节数
	long long buildUs = 0;		// 记录组装（实体序列化直写缓冲）总耗时（µs）
	long long fileUs = 0;		// 记录文件写（seekp + 整条 write + 校验码回写）总耗时（µs）
	long long flushUs = 0;		// flush 总耗时（µs）
	long long flushCnt = 0;		// flush 调用次数
	long long readCnt = 0;		// 磁盘读成功次数（selData 单条：读回 + 反序列化）
	long long readUs = 0;		// 磁盘读总耗时（µs，含文件开关与反序列化）
	// —— miss 归类探针（缓存未命中后磁盘 selData 单条的去向）——
	long long selCalls = 0;		// selData 单条进入次数（≈ 缓存 miss 数，取快照时可能含未执行任务）
	long long selInDiskMiss = 0;// 未开文件即 inDisk 缺失：瞬态（已淘汰脏数据尚未落盘）+ 真不存在
	long long selFileFail = 0;	// 开文件后失败总数（= 下列细分之和）
	long long selFailCheck = 0;	// 首字节校验码不符 / 读头失败（offset 可能越界/错位）
	long long selFailName = 0;	// 记录内嵌名字与查询名不符（offset 指向别条记录/截断头）
	long long selFailEof = 0;	// 实体区读超 EOF（记录长度与文件不符）
	long long selFailType = 0;	// 类型未注册
	long long selFailOpen = 0;	// 文件打开失败
	long long selOk = 0;		// 读盘成功（与 readCnt 同义，单独计数便于核对）
	long long compactCnt = 0;	// 自动压缩（容量超限触发的 reWrite）次数
	long long compactFail = 0;	// 自动压缩失败次数（失败会留下陈旧偏移 → 读失败来源）
	long long compactUs = 0;	// 压缩总耗时（µs；只统计成功完成的压缩）
	long long compactBefore = 0;// 压缩前文件逻辑大小累计（字节；用于算空洞回收量）
	long long compactAfter = 0;	// 压缩后文件大小累计（字节 = 各次 writePos 之和）
	long long overwriteCnt = 0;	// 原位覆盖写总次数（放得下旧槽时覆盖，未追加）
	long long overwriteSame = 0;	// 覆盖且等长（剩余=0，不产生洞）
	long long overwriteShrink = 0;	// 覆盖且缩小（剩余≥86，登记剩余洞）
	long long holeCnt = 0;		// 当前空洞段数（快照）
	long long holeBytes = 0;	// 当前空洞总字节（快照，含 86B 头）
	long long holeUseCnt = 0;	// 空洞复用次数（写入已有洞段，未追加）
};

class Disk {
	friend class Cache;
private:
	std::weak_ptr<Cache> cache;	// 缓存对象指针，用weak防止内存泄露
	long long maxSize;		// 磁盘容量上限（写路径强制执行：追加将超限时先自动压缩 reWrite 回收空洞，
							//   压缩后仍放不下则返回 MEMO_OUT；0 表示不设限）
	long long curSize;		// 磁盘当前容量（文件实际大小：追加增长，压缩后重置为紧凑长度）
	std::string dbName;		// 数据库名称(文件名)
	std::unordered_map<std::string, long long> inDisk;	// 变量名 + 记录偏移（64 位：数据文件可超 2GB）

	// ===== 磁盘 IO（仅磁盘线程访问，天然串行，无需锁）=====
	std::fstream file;			// 常驻文件句柄（读+写：所有持久化/重写复用，免每次 open/close）
	std::vector<char> recBuf;	// 单条记录组装缓冲（定长头 + 实体字节，序列化直写、整条一次落盘）

	// ===== 观测统计（性能归因探针，已用 #if 1 禁用；恢复观测删除 #if 1 / #endif 两行即可）=====
#if 1
	// 观测统计（relaxed 累加，磁盘线程单侧写，测试侧读）
	std::atomic<long long> statWriteCnt{ 0 };
	std::atomic<long long> statWriteBytes{ 0 };
	std::atomic<long long> statBuildUs{ 0 };	// 组装（序列化）耗时
	std::atomic<long long> statFileUs{ 0 };		// 记录文件写耗时
	std::atomic<long long> statFlushUs{ 0 };	// flush 耗时
	std::atomic<long long> statFlushCnt{ 0 };
	std::atomic<long long> statReadCnt{ 0 };
	std::atomic<long long> statReadUs{ 0 };
	// miss 归类探针计数
	std::atomic<long long> statSelCalls{ 0 };
	std::atomic<long long> statSelInDiskMiss{ 0 };
	std::atomic<long long> statSelFileFail{ 0 };
	std::atomic<long long> statSelFailCheck{ 0 };
	std::atomic<long long> statSelFailName{ 0 };
	std::atomic<long long> statSelFailEof{ 0 };
	std::atomic<long long> statSelFailType{ 0 };
	std::atomic<long long> statSelFailOpen{ 0 };
	std::atomic<long long> statSelOk{ 0 };
	std::atomic<long long> statCompactCnt{ 0 };
	std::atomic<long long> statCompactFail{ 0 };
	std::atomic<long long> statCompactUs{ 0 };
	std::atomic<long long> statCompactBefore{ 0 };
	std::atomic<long long> statCompactAfter{ 0 };
	std::atomic<long long> statOverwriteCnt{ 0 };
	std::atomic<long long> statOverwriteSame{ 0 };
	std::atomic<long long> statOverwriteShrink{ 0 };
	std::atomic<long long> statHoleCnt{ 0 };	// 当前空洞段数（快照用）
	std::atomic<long long> statHoleBytes{ 0 };	// 当前空洞总字节（快照用，含 86B 头）
	std::atomic<long long> statHoleUseCnt{ 0 };	// 空洞复用次数（累计）
#endif

	// ===== 空洞（删除=空洞，见 空洞删除段设计.md）=====
	// 空闲段索引：按容量升序 (容量=86+dataSize, 偏移)，best-fit 取用；仅磁盘线程读写
	std::multiset<std::pair<long long, long long>> holes;

	// 惰性打开/IO 错误后重建句柄：确保 file 已打开且可用（仅在写路径调用）
	bool ensureFileOpen();
	// 组装单条记录到 recBuf：定长头（校验码由 code 参数给定）+ 实体字节（直接序列化进缓冲）
	// 成功返回 SUCCESS 并写出 dataSize（实体字节数）/ recLen（整条长度，= recBuf.size()）
	int buildRecord(const std::string& varName, const Val& val, char code, int& dataSize);
	// 追加写单条记录（两段式校验码：先整条 CHECK_BROKEN，写毕回写 CHECK_VALID）
	// 不 flush（调用方按批 flush）；成功更新 inDisk/curSize
	int appendRecord(const std::string& varName, const Val& val);
	// 原位覆盖：已有旧记录且新记录放得下旧槽（含写剩余洞头）→ 覆盖返回 true（不追加、不增长）；
	// 否则返回 false（回退追加）。调用前提：recBuf 已组装好
	bool tryOverwriteInPlace(const std::string& varName);
	// 空洞复用：从 holes 取 ≥ 新长的最小洞（best-fit）写入 → 返回 true（不追加、不增长）；
	// 无可用洞返回 false（回退追加）。剩余 ≥86 再切洞登记
	bool tryUseHole(const std::string& varName);
	// 登记一个空洞段（容量=86+dataSize，偏移=offset）：写入 holes 并更新观测
	void addHole(long long offset, long long capacity);
	// 清除空洞索引（压缩成功/启动重建后：紧凑文件无洞）
	void clearHoles();
	// flush 并记账（调用方各写路径共用）
	void flushSync();
	// 把某偏移处的记录标记为已删/空洞（写 1 字节 CHECK_DELETED，旧头 dataSize 保留→可跳读/复用）；
	// 不负责 flush，由调用方随任务批 flush（见 空洞删除段设计.md §3.2）
	void markDeleted(long long offset);

public:
	explicit Disk(const long long& maxSize, std::string& dbName);
	~Disk();

	/* ========== special operation 特殊操作 ========== */

	inline bool containsVar(const std::string& varName) { return inDisk.contains(varName); }

	// 观测快照：磁盘 IO 阶段统计（序列化/文件写/flush/读）
	DiskIoStat getIoStat() const;

	/* ========== operation function(操作函数) ========== */

	// 绑定缓存对象
	void setCache(std::shared_ptr<Cache> c);
	// 删除某变量
	int delData(const std::string& varName);
	// 批量删除变量（集合版，一次提交）
	int delData(std::vector<std::string> varNameSet);
	// 持久化某变量
	int persisData(const std::string& varName);
	// 持久化某变量（带数据版：数据由调用方提供，不查缓存，供淘汰刷盘用）
	int persisData(const std::string& varName, const Val& val);
	// 持久化变量集（带数据批量版：一次文件开关写整批，数据由调用方提供，供淘汰攒批刷盘用）
	int persisData(std::vector<std::pair<std::string, Val>> dataSet);
	// 持久化某变量集
	int persisData(std::vector<std::string> varNameSet);
	// 持久化某个数据
	
	// 持久化所有数据
	int persisAll();
	// 查询某变量
	int selData(const std::string& varName, std::any& res, Val& val);
	// 查询多个变量
	int selData(std::vector<std::string>& varNameSet, std::vector<std::any>& resSet, std::vector<Val>& vals);
	// 刷盘
	int flushDisk();
	// 数据重写：缓存+inDisk 数据写入新文件，删旧文件，更新 inDisk，清空 delDisk
	int reWrite();	

};

#endif // !_DISK_H_
