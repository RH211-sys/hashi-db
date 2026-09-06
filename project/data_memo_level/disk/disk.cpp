#include "disk.h"
#include "../cache/cache.h"	// 方法实现需访问 Cache 完整类型（友元 + 私有成员）
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <streambuf>

/*
    <fstream> 用于读写文件内容
    <filesystem> 用于操作文件和目录本身。
*/

namespace {
    // 追加写 vector<char> 的流缓冲：cereal 序列化直接进连续记录缓冲，免 stringstream/二次整块拷贝
    class VecBuf : public std::streambuf {
        std::vector<char>& out;
    public:
        explicit VecBuf(std::vector<char>& o) : out(o) {}
    protected:
        int_type overflow(int_type ch) override {
            out.emplace_back(static_cast<char>(traits_type::to_char_type(ch)));
            return ch;
        }
        std::streamsize xsputn(const char* s, std::streamsize n) override {
            out.insert(out.end(), s, s + n);
            return n;
        }
    };
    // 配 VecBuf 的输出流（成员初始化顺序：先 m_buf 后基类无法保证，故基类先置空再 rdbuf 接管）
    class VecStream : public std::ostream {
        VecBuf m_buf;
    public:
        explicit VecStream(std::vector<char>& o) : std::ostream(nullptr), m_buf(o) { rdbuf(&m_buf); }
    };

    // 记录头偏移（与 protocol.h 布局一致，HEAD_SIZE = 86）
    // code | dataSize(4) | updateUS(8) | expireUS(8) | isPermanent(1) | typeBuf(32) | nameBuf(32) | 实体字节
    constexpr std::size_t HEAD_DS = CODE_LEN;
    constexpr std::size_t HEAD_UPDATE = HEAD_DS + ENTITY_SIZE_LEN;
    constexpr std::size_t HEAD_EXPIRE = HEAD_UPDATE + UPDATE_TIME_LEN;
    constexpr std::size_t HEAD_PERM = HEAD_EXPIRE + EXPIRE_TIME_LEN;
    constexpr std::size_t HEAD_TYPE = HEAD_PERM + IS_PERMANENT_LEN;
    constexpr std::size_t HEAD_NAME = HEAD_TYPE + TYPE_LEN;
    constexpr std::size_t HEAD_SIZE = HEAD_NAME + NAME_LEN;

    // 磁盘读失败抽样打印（探针：只打印前若干次，输出 ASCII）
    // codeByte/recSize 未知时传 -1；fileLen 现场量取
    void reportSelFail(const std::string& dbName, const char* kind, const std::string& key,
        long long offset, int codeByte, long long recSize) {
        static std::atomic<int> remain{ 10 };
        if (remain.fetch_sub(1) <= 0) return;
        long long fileLen = -1;
        {
            std::ifstream f(dbName, std::ios::binary);
            if (f) { f.seekg(0, std::ios::end); fileLen = static_cast<long long>(f.tellg()); }
        }
        std::cout << "[SEL-FAIL] kind=" << kind << " key=" << key << " offset=" << offset
            << " codeByte=" << codeByte << " recSize=" << recSize << " fileLen=" << fileLen << std::endl;
    }

    // 压缩(reWrite)失败点定位打印（独立额度，诊断用；诊断完可移除）
    void reportRewriteFail(const std::string& dbName, const char* why, const std::string& key, long long off, long long recLen = -1) {
        static std::atomic<int> remain{ 30 };
        if (remain.fetch_sub(1) <= 0) return;
        long long fileLen = -1;
        {
            std::ifstream f(dbName, std::ios::binary);
            if (f) { f.seekg(0, std::ios::end); fileLen = static_cast<long long>(f.tellg()); }
        }
        std::cout << "[REWRITE-FAIL] why=" << why << " key=" << key << " off=" << off
            << " recLen=" << recLen << " fileLen=" << fileLen << std::endl;
    }
}


Disk::Disk(const long long& maxSize, std::string& dbName)
{
    this->curSize = 0;
    this->dbName = dbName;
    this->maxSize = maxSize;

}

Disk::~Disk() = default;

void Disk::setCache(std::shared_ptr<Cache> c)
{
    cache = c;
}

bool Disk::ensureFileOpen()
{
    if (file.is_open() && file.good()) return true;
    if (file.is_open()) file.close();	// 打开但状态异常（上次 IO 失败）：关闭重建
    // 文件不存在时 in|out 打不开：先以 app 模式创建（保持既有"首次写建文件"语义）
    std::ofstream touch(dbName, std::ios::binary | std::ios::app);
    touch.close();
    file.open(dbName, std::ios::in | std::ios::out | std::ios::binary);
    return file.is_open() && file.good();
}

int Disk::buildRecord(const std::string& varName, const Val& val, char code, int& dataSize)
{
    /* ========== 基本信息校验 =========== */
    // 变量名超长：定长字段放不下，无法写入
    if (varName.size() > NAME_LEN) return LONG_NAME;
    // 查类型注册表（any 类型擦除，运行时只能按 typeName 查表拿模板实例）
    auto reg = typeReg.find(val.typeName);
    if (reg == typeReg.end()) return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）
    // 类型名超长：定长字段放不下，截断后无法反序列化
    if (val.typeName.size() > TYPE_LEN) return LONG_NAME;

    /* ========== 组装整条记录 =========== */
    // 定长头占位（全零 = 0x00，恰为 CHECK_BROKEN），实体字节随后直接序列化进同一缓冲
    recBuf.clear();
    recBuf.resize(HEAD_SIZE);
    auto t0 = std::chrono::steady_clock::now();
    {
        VecStream os(recBuf);
        reg->second.first(*val.entity, os);	// 实体字节追加到 recBuf[HEAD_SIZE..]，零中间拷贝
    }
    statBuildUs.fetch_add(static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);	// 组装（序列化）耗时记账
    dataSize = static_cast<int>(recBuf.size() - HEAD_SIZE);	// 数据大小：只含实体

    // 时间信息（updateTime 已是 µs 整数；expireTime 仍为 time_point）
    long long updateUS = val.updateTime.load(std::memory_order_relaxed);
    long long expireUS = val.isPermanent ? 0
        : std::chrono::duration_cast<std::chrono::microseconds>(val.expireTime.time_since_epoch()).count();

    char* p = recBuf.data();
    p[0] = code;
    memcpy(p + HEAD_DS, &dataSize, ENTITY_SIZE_LEN);
    memcpy(p + HEAD_UPDATE, &updateUS, UPDATE_TIME_LEN);
    memcpy(p + HEAD_EXPIRE, &expireUS, EXPIRE_TIME_LEN);
    memcpy(p + HEAD_PERM, &val.isPermanent, IS_PERMANENT_LEN);
    memcpy(p + HEAD_TYPE, val.typeName.c_str(), val.typeName.size());	// 其余由 resize 清零补足定长
    memcpy(p + HEAD_NAME, varName.c_str(), varName.size());
    return SUCCESS;
}

int Disk::appendRecord(const std::string& varName, const Val& val)
{
    if (!ensureFileOpen()) return FILE_OPEN_FILED;	// 常驻句柄不可用

    int dataSize = 0;
    int code = buildRecord(varName, val, CHECK_BROKEN, dataSize);
    if (code != SUCCESS) return code;

    // 原位覆盖优先：该 key 已有磁盘记录且新记录放得下旧槽 → 覆盖写（不追加、不增长、不触发容量压缩）
    if (tryOverwriteInPlace(varName)) return SUCCESS;

    // 容量执行（磁盘上限 = 配置的 maxSize，由业务侧设置）：
    // 追加后超过上限 → 先同线程压缩（reWrite 回收空洞并截断），压缩后仍放不下才拒绝(MEMO_OUT)
    // 注1：reWrite 会复用 recBuf，故压缩后需重建本记录
    // 注2：压缩前必须 flushSync —— 本任务此前追加的记录可能还在流缓冲里（批量写到一半触发），
    //      reWrite 需从文件读回这些记录（read-move），未落盘内容会读空导致压缩失败
    if (maxSize > 0 && curSize + static_cast<long long>(recBuf.size()) > maxSize) {
        flushSync();
        if (!file) return UNKNOWN_ERROR;
        statCompactCnt.fetch_add(1, std::memory_order_relaxed);	// 观测：自动压缩次数
        code = reWrite();
        if (code != SUCCESS) {	// 压缩失败：文件可能半新半旧（陈旧偏移的来源），本次追加中止
            statCompactFail.fetch_add(1, std::memory_order_relaxed);
            reportSelFail(dbName, "compact-fail", varName, 0, code, -1);
            return code;
        }
        code = buildRecord(varName, val, CHECK_BROKEN, dataSize);
        if (code != SUCCESS) return code;
        if (curSize + static_cast<long long>(recBuf.size()) > maxSize) return MEMO_OUT;	// 压缩后仍超限：容量不足
    }

    // 一律追加写：旧记录无法保证长度一致，覆盖会产生碎片，留空洞等重写回收
    auto t1 = std::chrono::steady_clock::now();
    file.seekp(0, std::ios::end);
    std::streamoff offset = file.tellp();	// 记录偏移 = 文件当前大小
    if (!file) { file.clear(); return UNKNOWN_ERROR; }

    // 两段式写：整条一次写入（头含无效校验码），写毕回写有效码（写一半崩溃 → 校验码无效，重建时截断）
    file.write(recBuf.data(), static_cast<std::streamsize>(recBuf.size()));
    file.seekp(offset);						// 回写有效校验码
    char valid = CHECK_VALID;
    file.write(&valid, CODE_LEN);
    if (!file) return UNKNOWN_ERROR;
    statFileUs.fetch_add(static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t1).count()), std::memory_order_relaxed);	// 文件写耗时记账
    statWriteCnt.fetch_add(1, std::memory_order_relaxed);
    statWriteBytes.fetch_add(static_cast<long long>(recBuf.size()), std::memory_order_relaxed);

    // 同名变量可能有多条记录（旧版本），inDisk 指向最新一条；重建时同名覆盖，旧记录成空洞
    inDisk[varName] = static_cast<long long>(offset);
    curSize += static_cast<long long>(recBuf.size());	// 只增不减：空洞/已删数据不回收，重写时重新统计
    return SUCCESS;
}

void Disk::flushSync()
{
    // flush 并记账（写路径共用：单条持久化/批量末尾/重写末尾）
    auto t0 = std::chrono::steady_clock::now();
    file.flush();
    statFlushUs.fetch_add(static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
    statFlushCnt.fetch_add(1, std::memory_order_relaxed);
}

void Disk::addHole(long long offset, long long capacity)
{
    // 登记空闲段（仅磁盘线程调用）；容量 = 86 + dataSize，best-fit 取用
    holes.emplace(capacity, offset);
    statHoleCnt.fetch_add(1, std::memory_order_relaxed);
    statHoleBytes.fetch_add(capacity, std::memory_order_relaxed);
}

void Disk::clearHoles()
{
    // 压缩成功/重建后：紧凑文件无洞
    if (!holes.empty()) holes.clear();
    statHoleCnt.store(0, std::memory_order_relaxed);
    statHoleBytes.store(0, std::memory_order_relaxed);
}

void Disk::markDeleted(long long offset)
{
    // 已删/空洞标记（1 字节 CHECK_DELETED）+ 登记空洞：旧头 dataSize 保留 → 可跳读/复用
    if (!ensureFileOpen()) return;
    file.seekg(offset);
    char oldCheck = 0;
    int oldDataSize = 0;
    file.read(&oldCheck, CODE_LEN);
    file.read((char*)&oldDataSize, ENTITY_SIZE_LEN);
    if (!file || oldCheck != CHECK_VALID) { file.clear(); return; }	// 异常/已删：不重复登记
    file.seekp(offset);
    char code = CHECK_DELETED;
    file.write(&code, CODE_LEN);
    if (!file) { file.clear(); return; }	// 标记失败：逻辑删除仍生效（inDisk 已擦除）；loader 阶段按文件为准
    addHole(offset, static_cast<long long>(HEAD_SIZE) + oldDataSize);
}

bool Disk::tryOverwriteInPlace(const std::string& varName)
{
    // 原位覆盖（前提：recBuf 已组装）：
    //   inDisk 有旧记录且新记录总长 ≤ 旧槽总长，且（等长 或 剩余 ≥ 一个洞头）→ 覆盖写（+剩余区洞头）
    //   返回 true：不追加、文件不增长、inDisk 偏移不变、无需容量检查
    auto it = inDisk.find(varName);
    if (it == inDisk.end()) return false;
    long long oldOff = it->second;
    long long newTotal = static_cast<long long>(recBuf.size());
    if (!ensureFileOpen()) return false;

    // 读旧头：VALID 校验 + 旧 dataSize（决定旧槽容量）
    file.seekg(oldOff);
    char oldCheck = 0;
    int oldDataSize = 0;
    file.read(&oldCheck, CODE_LEN);
    file.read((char*)&oldDataSize, ENTITY_SIZE_LEN);
    if (!file || oldCheck != CHECK_VALID) { file.clear(); return false; }
    long long oldTotal = static_cast<long long>(HEAD_SIZE) + oldDataSize;
    if (newTotal > oldTotal) return false;					// 放不下 → 回退追加
    long long remain = oldTotal - newTotal;
    if (remain != 0 && remain < HEAD_SIZE) return false;	// 剩余不足一个洞头：链会断 → 回退追加

    auto t1 = std::chrono::steady_clock::now();
    // 两段式覆盖：recBuf 头为 BROKEN → 整条写 → 回写 VALID
    file.seekp(oldOff);
    file.write(recBuf.data(), static_cast<std::streamsize>(recBuf.size()));
    file.seekp(oldOff);
    char valid = CHECK_VALID;
    file.write(&valid, CODE_LEN);
    if (remain >= HEAD_SIZE) {
        // 剩余区写洞头：code=DELETED + dataSize=剩余容量-头（其余头字段不填，跳读只用 code+size）
        long long holeDataSize = remain - HEAD_SIZE;
        file.seekp(oldOff + newTotal);
        char del = CHECK_DELETED;
        file.write(&del, CODE_LEN);
        int sz = static_cast<int>(holeDataSize);
        file.write((char*)&sz, ENTITY_SIZE_LEN);
    }
    if (!file) {
        // 覆盖失败：恢复旧校验码，保持文件可解析（被覆盖内容不再被引用）
        file.clear();
        file.seekp(oldOff);
        file.write(&oldCheck, CODE_LEN);
        return false;
    }
    if (remain >= HEAD_SIZE) addHole(oldOff + newTotal, remain);	// 登记剩余洞
    statFileUs.fetch_add(static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t1).count()), std::memory_order_relaxed);
    statWriteCnt.fetch_add(1, std::memory_order_relaxed);
    statOverwriteCnt.fetch_add(1, std::memory_order_relaxed);
    if (remain == 0) statOverwriteSame.fetch_add(1, std::memory_order_relaxed);
    else statOverwriteShrink.fetch_add(1, std::memory_order_relaxed);
    return true;	// inDisk/curSize 不变；flush 由调用方批处理
}

DiskIoStat Disk::getIoStat() const
{
    // 观测快照：relaxed 读即可（累加值，不强一致）
    DiskIoStat s;
    s.writeCnt = statWriteCnt.load(std::memory_order_relaxed);
    s.writeBytes = statWriteBytes.load(std::memory_order_relaxed);
    s.buildUs = statBuildUs.load(std::memory_order_relaxed);
    s.fileUs = statFileUs.load(std::memory_order_relaxed);
    s.flushUs = statFlushUs.load(std::memory_order_relaxed);
    s.flushCnt = statFlushCnt.load(std::memory_order_relaxed);
    s.readCnt = statReadCnt.load(std::memory_order_relaxed);
    s.readUs = statReadUs.load(std::memory_order_relaxed);
    s.selCalls = statSelCalls.load(std::memory_order_relaxed);
    s.selInDiskMiss = statSelInDiskMiss.load(std::memory_order_relaxed);
    s.selFileFail = statSelFileFail.load(std::memory_order_relaxed);
    s.selFailCheck = statSelFailCheck.load(std::memory_order_relaxed);
    s.selFailName = statSelFailName.load(std::memory_order_relaxed);
    s.selFailEof = statSelFailEof.load(std::memory_order_relaxed);
    s.selFailType = statSelFailType.load(std::memory_order_relaxed);
    s.selFailOpen = statSelFailOpen.load(std::memory_order_relaxed);
    s.selOk = statSelOk.load(std::memory_order_relaxed);
    s.compactCnt = statCompactCnt.load(std::memory_order_relaxed);
    s.compactFail = statCompactFail.load(std::memory_order_relaxed);
    s.compactUs = statCompactUs.load(std::memory_order_relaxed);
    s.compactBefore = statCompactBefore.load(std::memory_order_relaxed);
    s.compactAfter = statCompactAfter.load(std::memory_order_relaxed);
    s.overwriteCnt = statOverwriteCnt.load(std::memory_order_relaxed);
    s.overwriteSame = statOverwriteSame.load(std::memory_order_relaxed);
    s.overwriteShrink = statOverwriteShrink.load(std::memory_order_relaxed);
    s.holeCnt = statHoleCnt.load(std::memory_order_relaxed);
    s.holeBytes = statHoleBytes.load(std::memory_order_relaxed);
    return s;
}


int Disk::delData(const std::string& varName)
{
    // 删除 = 空洞：擦 inDisk + 文件头 1 字节 CHECK_DELETED（dataSize 保留 → 可跳读/复用）
    auto it = inDisk.find(varName);
    if (it == inDisk.end()) {
        return FIND_FAILED;	// 该数据不存在，可能已经被删除了
    }
    long long offset = it->second;
    inDisk.erase(it);
    markDeleted(offset);
    if (file.is_open()) file.flush();	// 标记随任务落盘（不计入 flush 观测）
    return SUCCESS;
}

int Disk::delData(std::vector<std::string> varNameSet)
{
    // 批量删除：逐条擦 inDisk + 文件头标记，结束时一次 flush
    bool any = false;
    for (const auto& varName : varNameSet) {
        auto it = inDisk.find(varName);
        if (it != inDisk.end()) {
            long long offset = it->second;
            inDisk.erase(it);
            markDeleted(offset);
            any = true;
        }	// 不存在则无操作，不视为错误
    }
    if (any && file.is_open()) file.flush();	// 标记随任务落盘（不计入 flush 观测）
    return SUCCESS;
}


int Disk::persisData(const std::string& varName)
{
    /* ========== 检查缓存是否存在 =========== */

    // 从缓存拿 Val（缓存是权威）：weak_ptr 提升拿缓存指针（非锁，不阻塞），读锁内拷贝，序列化/写文件在锁外
    auto c = cache.lock();
    if (!c) return UNKNOWN_ERROR;	// 缓存未绑定或已析构

    /* ========== 检查缓存是否存在该数据 =========== */

    Val val;
    {
        c->rwMutex->lock_shared();	// 只有访问缓存才需要锁：锁内只做拷贝，短临界区
        auto it = c->cache_db.find(varName);
        if (it == c->cache_db.end()) {
            c->rwMutex->unlock_shared();
            return FIND_FAILED;	// 缓存没有该数据，无从持久化
        }
        val = it->second;	// 拷贝 Val，锁外使用
        c->rwMutex->unlock_shared();
    }

    /* ========== 检查基本信息是否正确 =========== */

    // 变量名超长：定长字段放不下，无法写入
    if (varName.size() > NAME_LEN) return LONG_NAME;

    // 查类型注册表，序列化实体（any 类型擦除，运行时只能按 typeName 查表拿模板实例）
    auto reg = typeReg.find(val.typeName);
    if (reg == typeReg.end()) return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）

    // 类型名超长：定长字段放不下，截断后无法反序列化
    if (val.typeName.size() > TYPE_LEN) return LONG_NAME;

    /* ========== 组装 + 追加写 =========== */

    // 写文件前先清 isDirty（写锁内）：防止写文件期间缓存被改（新脏数据）后误清
    // 若写盘失败则置回 true，等待下次重刷
    {
        c->rwMutex->lock();
        auto it = c->cache_db.find(varName);
        if (it != c->cache_db.end()) {
            it->second.isDirty = false;
        }
        c->rwMutex->unlock();
    }

    // 单条持久化 = 显式落盘点：组装（定长头+实体直写缓冲）→ 整条一次写 → flush
    int code = appendRecord(varName, val);
    if (code == SUCCESS) {
        flushSync();
        if (!file) code = UNKNOWN_ERROR;	// flush 失败视同写失败
    }
    if (code != SUCCESS) {
        // 写失败：isDirty 置回 true，等待下次重刷
        {
            c->rwMutex->lock();
            auto it = c->cache_db.find(varName);
            if (it != c->cache_db.end()) {
                it->second.isDirty = true;
            }
            c->rwMutex->unlock();
        }
        return code;
    }
    return SUCCESS;
}

int Disk::persisData(const std::string& varName, const Val& val)
{
    // 与无数据版流程一致，但数据由调用方提供：不查缓存、不动 isDirty（淘汰时缓存已删）

    /* ========== 检查基本信息是否正确 =========== */

    // 变量名超长：定长字段放不下，无法写入
    if (varName.size() > NAME_LEN) return LONG_NAME;

    // 查类型注册表（any 类型擦除，运行时只能按 typeName 查表拿模板实例）
    auto reg = typeReg.find(val.typeName);
    if (reg == typeReg.end()) return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）

    // 类型名超长：定长字段放不下，截断后无法反序列化
    if (val.typeName.size() > TYPE_LEN) return LONG_NAME;

    /* ========== 组装 + 追加写 =========== */

    // 数据已从缓存删除，无 isDirty 可清/可恢复：组装 → 整条一次写 → flush（失败无法重试，调用方自行处理）
    int code = appendRecord(varName, val);
    if (code == SUCCESS) {
        flushSync();
        if (!file) code = UNKNOWN_ERROR;	// 写入失败（数据已从缓存删，无法重试）
    }
    return code;
}

int Disk::persisData(std::vector<std::pair<std::string, Val>> dataSet)
{
    if (dataSet.empty()) return SUCCESS;

    // 一次常驻句柄写多条数据：批量落盘，减少物理磁盘 IO 频率
    // （写入先进页缓存，多个数据可能合并为一次物理落盘；组装+整条一次写见 appendRecord）
    int firstError = SUCCESS;	// 记录首个错误码，其余变量继续处理
    for (const auto& item : dataSet) {
        int code = appendRecord(item.first, item.second);
        if (code == MEMO_OUT) {	// 容量已满：压缩后仍放不下，继续只会反复触发压缩
            if (firstError == SUCCESS) firstError = code;
            break;
        }
        if (code != SUCCESS && firstError == SUCCESS) {
            firstError = code;
        }
    }

    if (file.is_open()) {
        flushSync();	// 整批一次 flush
        if (!file) {
            return UNKNOWN_ERROR;	// 写入失败（数据已从缓存删，无法重试，调用方自行处理）
        }
    }
    return firstError;
}

int Disk::persisData(std::vector<std::string> varNameSet)
{
    if (varNameSet.empty()) return SUCCESS;

    // 从缓存拿缓存指针（非锁，不阻塞）
    auto c = cache.lock();
    if (!c) return UNKNOWN_ERROR;	// 缓存未绑定或已析构

    // 已清 isDirty 的变量名：批量写失败时统一置回 true
    std::vector<std::string> clearedNames;

    // 单条写入（流程同单变量版）：成功返回 SUCCESS 并更新 inDisk/curSize，失败返回错误码，其余变量继续处理
    auto writeOne = [&](const std::string& varName) -> int {
        /* ========== 检查缓存是否存在该数据 =========== */
        Val val;
        {
            c->rwMutex->lock_shared();	// 只有访问缓存才需要锁：锁内只做拷贝，短临界区
            auto it = c->cache_db.find(varName);
            if (it == c->cache_db.end()) {
                c->rwMutex->unlock_shared();
                return FIND_FAILED;	// 缓存没有该数据，无从持久化
            }
            val = it->second;	// 拷贝 Val，锁外使用
            c->rwMutex->unlock_shared();
        }

        /* ========== 检查基本信息是否正确 =========== */
        // 变量名超长：定长字段放不下，无法写入
        if (varName.size() > NAME_LEN) return LONG_NAME;
        // 查类型注册表（any 类型擦除，运行时只能按 typeName 查表拿模板实例）
        auto reg = typeReg.find(val.typeName);
        if (reg == typeReg.end()) return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）
        // 类型名超长：定长字段放不下，截断后无法反序列化
        if (val.typeName.size() > TYPE_LEN) return LONG_NAME;

        /* ========== 追加写入 =========== */
        // 写盘前先清 isDirty（写锁内）：防止写文件期间缓存被改（新脏数据）后误清
        // 若批量写失败则统一置回 true，等待下次重刷
        {
            c->rwMutex->lock();
            auto it = c->cache_db.find(varName);
            if (it != c->cache_db.end()) {
                it->second.isDirty = false;
            }
            c->rwMutex->unlock();
            clearedNames.emplace_back(varName);
        }

        // 组装 + 整条一次写（校验已在上方完成；IO 类失败由末尾统一 flush 检查兜底）
        return appendRecord(varName, val);
    };

    /* ========== 循环写入 =========== */
    int firstError = SUCCESS;	// 记录首个错误码，其余变量继续处理
    for (const auto& varName : varNameSet) {
        int code = writeOne(varName);
        if (code == MEMO_OUT) {	// 容量已满：压缩后仍放不下，中断本批（避免逐条反复压缩）
            if (firstError == SUCCESS) firstError = code;
            break;
        }
        if (code != SUCCESS && firstError == SUCCESS) {
            firstError = code;
        }
    }

    if (firstError == MEMO_OUT) {
        // 容量不足：已清的 isDirty 全部置回 true，等待磁盘腾出空间后重刷
        {
            c->rwMutex->lock();
            for (const auto& n : clearedNames) {
                auto it = c->cache_db.find(n);
                if (it != c->cache_db.end()) {
                    it->second.isDirty = true;
                }
            }
            c->rwMutex->unlock();
        }
        return MEMO_OUT;
    }

    if (file.is_open()) {
        flushSync();	// 整批一次 flush
        if (!file) {
            // 批量写失败：已清的 isDirty 全部置回 true，等待下次重刷
            {
                c->rwMutex->lock();
                for (const auto& n : clearedNames) {
                    auto it = c->cache_db.find(n);
                    if (it != c->cache_db.end()) {
                        it->second.isDirty = true;
                    }
                }
                c->rwMutex->unlock();
            }
            return UNKNOWN_ERROR;	// 写入失败
        }
    }
    return firstError;
}

int Disk::persisAll()
{
    // 从缓存拿缓存指针（非锁，不阻塞）
    auto c = cache.lock();
    if (!c) return UNKNOWN_ERROR;	// 缓存未绑定或已析构

    // 收集未过期的脏数据变量名（读锁，锁内只做收集）：非脏不存、过期不存
    std::vector<std::string> dirtyNames;
    {
        c->rwMutex->lock_shared();
        auto now = std::chrono::system_clock::now();
        for (const auto& [name, val] : c->cache_db) {
            if (!val.isDirty) continue;									// 非脏数据不存
            if (!val.isPermanent && val.expireTime <= now) continue;	// 过期数据不存
            dirtyNames.emplace_back(name);
        }
        c->rwMutex->unlock_shared();
    }

    // 复用列表版批量写入（一次文件开关，写前清 isDirty，失败统一置回）
    return persisData(std::move(dirtyNames));
}

int Disk::selData(const std::string& varName, std::any& res, Val& val)
{
    statSelCalls.fetch_add(1, std::memory_order_relaxed);	// 观测：进入（缓存未命中 → 磁盘读）

    /* ========== 查内存索引 =========== */
    auto it = inDisk.find(varName);
    if (it == inDisk.end()) {
        statSelInDiskMiss.fetch_add(1, std::memory_order_relaxed);	// 观测：未开文件即缺失（含"已淘汰未落盘"瞬态与真不存在）
        return FIND_FAILED;	// 磁盘没有该数据
    }
    long long offset = it->second;

    // 读耗时观测起点（含文件开关 + 读记录 + 反序列化，成功读回才记账）
    auto t0 = std::chrono::steady_clock::now();

    /* ========== 打开文件读取（复用常驻读写句柄，免每次 open/close） =========== */
    if (!ensureFileOpen()) {
        statSelFileFail.fetch_add(1, std::memory_order_relaxed);
        statSelFailOpen.fetch_add(1, std::memory_order_relaxed);
        reportSelFail(dbName, "open", varName, offset, -1, -1);	// 文件打开失败
        return FILE_OPEN_FILED;
    }

    /* ========== 读记录头 =========== */
    file.seekg(offset);
    char check = 0;
    file.read(&check, CODE_LEN);
    if (!file || check != CHECK_VALID) {
        // 首字节校验码不符 / 头读失败（offset 越界或错位）——抽样打印现场
        statSelFileFail.fetch_add(1, std::memory_order_relaxed);
        statSelFailCheck.fetch_add(1, std::memory_order_relaxed);
        reportSelFail(dbName, "check", varName, offset,
            static_cast<int>(static_cast<unsigned char>(check)), -1);
        return FIND_FAILED;	// 校验码无效：数据不完整
    }

    int dataSize = 0;
    file.read((char*)&dataSize, ENTITY_SIZE_LEN);

    // 时间信息：微秒整数（8B），与 std::chrono 互转
    long long updateUS = 0, expireUS = 0;
    char isPermanent = 0;
    file.read((char*)&updateUS, UPDATE_TIME_LEN);
    file.read((char*)&expireUS, EXPIRE_TIME_LEN);
    file.read(&isPermanent, IS_PERMANENT_LEN);

    char typeBuf[TYPE_LEN] = { 0 };
    file.read(typeBuf, TYPE_LEN);
    char nameBuf[NAME_LEN] = { 0 };
    file.read(nameBuf, NAME_LEN);

    // 记录里的变量名应与查询名一致（防偏移错乱）
    if (memcmp(nameBuf, varName.c_str(), varName.size()) != 0) {
        statSelFileFail.fetch_add(1, std::memory_order_relaxed);
        statSelFailName.fetch_add(1, std::memory_order_relaxed);
        reportSelFail(dbName, "name", varName, offset, -1, dataSize);
        return FIND_FAILED;
    }

    /* ========== 读实体并反序列化 =========== */
    std::vector<char> bytes(dataSize);
    file.read(bytes.data(), dataSize);
    if (!file) {
        statSelFileFail.fetch_add(1, std::memory_order_relaxed);
        statSelFailEof.fetch_add(1, std::memory_order_relaxed);
        reportSelFail(dbName, "eof", varName, offset, -1, dataSize);	// 实体区读超 EOF
        return FIND_FAILED;	// 读取失败（数据不完整）
    }

    // 查类型注册表，反序列化实体（any 类型擦除，按 typeName 查表拿模板实例）
    std::string typeName(typeBuf);	// 截到 '\0'
    auto reg = typeReg.find(typeName);
    if (reg == typeReg.end()) {
        statSelFileFail.fetch_add(1, std::memory_order_relaxed);
        statSelFailType.fetch_add(1, std::memory_order_relaxed);
        reportSelFail(dbName, "type", varName, offset, -1, dataSize);	// 类型未注册
        return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）
    }
    res = reg->second.second(bytes);	// 反序列化 → any

    /* ========== 填时间信息 =========== */
    val.typeName = typeName;
    val.dataSize = dataSize;	// 数据大小：实体字节数（回填缓存用）
    val.updateTime.store(updateUS, std::memory_order_relaxed);	// 记录里的更新时间（µs）
    val.isPermanent = isPermanent;	// char 转 bool：非 0 即 true
    val.expireTime = val.isPermanent
        ? std::chrono::system_clock::time_point(std::chrono::microseconds(updateUS))
        : std::chrono::system_clock::time_point(std::chrono::microseconds(expireUS));
    statReadUs.fetch_add(static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);	// 磁盘读耗时记账
    statReadCnt.fetch_add(1, std::memory_order_relaxed);
    statSelOk.fetch_add(1, std::memory_order_relaxed);	// 观测：读盘成功
    return SUCCESS;
}

int Disk::selData(std::vector<std::string>& varNameSet, std::vector<std::any>& resSet, std::vector<Val>& vals)
{
	if (varNameSet.empty()) return SUCCESS;

	// 一次文件开关读取多条：批量读，减少物理磁盘 IO 频率（与批量持久化对称）
	std::ifstream file(dbName, std::ios::binary);
	if (!file) return FILE_OPEN_FILED;	// 文件打开失败

	// 单条读取（流程同单变量版）：成功才收集实体+时间信息，失败记录错误码，其余变量继续处理
	auto readOne = [&](const std::string& varName) -> int {
		/* ========== 查内存索引 =========== */
		auto it = inDisk.find(varName);
		if (it == inDisk.end()) return FIND_FAILED;	// 磁盘没有该数据

		/* ========== 读记录头 =========== */
		file.seekg(it->second);
		char check = 0;
		file.read(&check, CODE_LEN);
		if (!file || check != CHECK_VALID) return FIND_FAILED;	// 校验码无效：数据不完整

		int dataSize = 0;
		file.read((char*)&dataSize, ENTITY_SIZE_LEN);

		// 时间信息：微秒整数（8B），与 std::chrono 互转
		long long updateUS = 0, expireUS = 0;
		char isPermanent = 0;
        char typeBuf[TYPE_LEN] = { 0 };
        char nameBuf[NAME_LEN] = { 0 };
		file.read((char*)&updateUS, UPDATE_TIME_LEN);
		file.read((char*)&expireUS, EXPIRE_TIME_LEN);
		file.read(&isPermanent, IS_PERMANENT_LEN);
		file.read(typeBuf, TYPE_LEN);
		file.read(nameBuf, NAME_LEN);
		if (!file) return FIND_FAILED;	// 读取失败（数据不完整）

		// 记录里的变量名应与查询名一致（防偏移错乱）
		if (memcmp(nameBuf, varName.c_str(), varName.size()) != 0) return FIND_FAILED;

		/* ========== 读实体并反序列化 =========== */
		std::vector<char> bytes(dataSize);
		file.read(bytes.data(), dataSize);
		if (!file) return FIND_FAILED;	// 读取失败（数据不完整）

		// 查类型注册表，反序列化实体（any 类型擦除，按 typeName 查表拿模板实例）
		std::string typeName(typeBuf);	// 截到 '\0'
		auto reg = typeReg.find(typeName);
		if (reg == typeReg.end()) return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）

		/* ========== 收集结果 =========== */
		resSet.emplace_back(reg->second.second(bytes));	// 实体：反序列化 → any

		// 时间信息：与单变量版一致（永久数据的过期时间 = 更新时间，不使用）
		Val val;
		val.typeName = typeName;
		val.dataSize = dataSize;	// 数据大小：实体字节数（回填缓存用）
		val.updateTime.store(updateUS, std::memory_order_relaxed);	// 记录里的更新时间（µs）
		val.isPermanent = isPermanent;	// char 转 bool：非 0 即 true
		val.expireTime = val.isPermanent
			? std::chrono::system_clock::time_point(std::chrono::microseconds(updateUS))
			: std::chrono::system_clock::time_point(std::chrono::microseconds(expireUS));
		vals.emplace_back(std::move(val));
		return SUCCESS;
	};

	/* ========== 循环读取 =========== */
	int firstError = SUCCESS;	// 记录首个错误码，其余变量继续处理
	for (const auto& varName : varNameSet) {
		int code = readOne(varName);
		if (code != SUCCESS && firstError == SUCCESS) {
			firstError = code;
		}
	}
	return firstError;
}

int Disk::flushDisk()
{
    // 从缓存拿缓存指针（非锁，不阻塞）
    auto c = cache.lock();
    if (!c) return UNKNOWN_ERROR;	// 缓存未绑定或已析构

    /*
        1. 过期数据：缓存删除 + 磁盘索引删除 + 文件标记(删除=空洞，随 delData 登记)
        2. 未过期脏数据：收集变量名，锁外批量刷盘，防止锁堵住
    */
    std::vector<std::string> expiredNames;
    std::vector<std::string> flushNames;
    {
        c->rwMutex->lock();
        auto now = std::chrono::system_clock::now();    

        auto it = c->cache_db.begin();
        while (it != c->cache_db.end()) {
            auto& val = it->second;
            if (!val.isPermanent && val.expireTime <= now) {
                // 过期：直接删缓存（缓存权威，过期数据不落盘）；inDisk/文件标记由 delData 统一处理
                c->curSize -= val.dataSize;	// 更新缓存当前大小
                expiredNames.emplace_back(it->first);
                it = c->cache_db.erase(it);
            } else {
                if (val.isDirty) flushNames.emplace_back(it->first);	// 未过期脏数据：收集刷盘
                ++it;
            }
        }
        c->rwMutex->unlock();
    }
    // 过期清理：擦 inDisk + 文件头删除标记 + 登记空洞（批内一次 flush）
    if (!expiredNames.empty()) delData(std::move(expiredNames));
    /*
        复用persisData重载（一次文件开关，写前清 isDirty，失败统一置回）
        inDisk 中已存在的数据：追加/原位覆盖后偏移为最新，旧记录成空洞等重写回收
    */
    return persisData(std::move(flushNames));
}

int Disk::reWrite()
{
    // 两阶段原地压缩（无 tmp，磁盘利用率高）：
    //   阶段A：只搬"旧文件里有、缓存里没有（或缓存里干净）"的记录——整条读旧写新，长度不变，
    //          写游标 ≤ 读游标，永远不会覆盖尚未读取的记录；
    //   阶段B：缓存里的脏 key 最后写——它们序列化后可能变大，但此刻旧区已无任何待读记录，
    //          写游标不再需要保护未读数据，可安全覆盖/追加。
    // 为何两阶段：若把"会变大的缓存写"与"读旧搬移"按偏移交错进行，变大的写会造成写游标前跳，
    // 盖掉后面还没读的旧记录（写覆盖读）→ read-head 失败/索引陈旧（见此前 debug 记录）。
    // —— 压缩观测起点：耗时 + 压缩前文件大小（评估空洞回收，方向1）——
    auto compactT0 = std::chrono::steady_clock::now();
    const long long compactBeforeBytes = curSize;

    auto c = cache.lock();
    if (!c) return UNKNOWN_ERROR;	// 缓存未绑定或已析构

    // 先删过期缓存数据（缓存删 + 磁盘索引删，重写不搬移它们，防从文件复活）
    {
        c->rwMutex->lock();	// 有删除操作，需要写锁
        auto now = std::chrono::system_clock::now();
        auto it = c->cache_db.begin();
        while (it != c->cache_db.end()) {
            auto& val = it->second;
            if (!val.isPermanent && val.expireTime <= now) {
                c->curSize -= val.dataSize;	// 更新缓存当前大小
                inDisk.erase(it->first);	// 磁盘索引删：主循环不再搬移它
                it = c->cache_db.erase(it);
            } else {
                ++it;
            }
        }
        c->rwMutex->unlock();
    }

    // 常驻句柄（读旧 + 写紧凑），先提交未落盘内容
    if (!ensureFileOpen()) return FILE_OPEN_FILED;	// 文件打开失败
    flushSync();
    if (!file) return UNKNOWN_ERROR;
    std::fstream& file = this->file;

    // inDisk 偏移快照（按旧偏移升序）
    std::vector<std::pair<std::string, long long>> sorted(inDisk.begin(), inDisk.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    // 缓存脏 key 快照：这些最后从缓存写（不读旧）；其余（不在缓存 / 缓存里干净）阶段A读旧搬移。
    // 干净缓存 key 与磁盘一致 → 读旧搬移等价；若期间变脏，最多搬旧值，脏位仍在 → 后续刷盘以缓存覆盖（最终一致）
    std::unordered_set<std::string> cacheDirty;
    {
        c->rwMutex->lock_shared();
        cacheDirty.reserve(c->cache_db.size());
        for (const auto& [name, val] : c->cache_db) {
            if (val.isDirty) cacheDirty.insert(name);
        }
        c->rwMutex->unlock_shared();
    }

    long long writePos = 0;	// 写游标（紧凑位置）

    /* ========== 阶段A：非缓存脏 key，读旧 → 紧凑位写（长度不变，写游标追不上读游标） ========== */
    for (const auto& [name, offset] : sorted) {
        if (cacheDirty.contains(name)) continue;	// 脏 key 留到阶段B（可能变大）

        // 读旧记录头
        file.seekg(offset);
        char check = 0;
        int dataSize = 0;
        file.read(&check, CODE_LEN);
        file.read((char*)&dataSize, ENTITY_SIZE_LEN);
        if (!file || check != CHECK_VALID) {
            reportRewriteFail(dbName, "read-head", name, offset);
            return UNKNOWN_ERROR;	// 旧文件异常（健康文件不会走到这里）
        }
        long long recLen = static_cast<long long>(CODE_LEN + ENTITY_SIZE_LEN + TIME_INFO_LEN + TYPE_LEN + NAME_LEN) + dataSize;
        std::vector<char> buf(static_cast<size_t>(recLen));
        file.seekg(offset);
        file.read(buf.data(), static_cast<std::streamsize>(recLen));
        if (!file) {
            reportRewriteFail(dbName, "read-rec", name, offset, recLen);
            return UNKNOWN_ERROR;
        }

        // 写紧凑位
        file.seekp(writePos);
        file.write(buf.data(), static_cast<std::streamsize>(recLen));
        if (!file) {
            reportRewriteFail(dbName, "write-rec", name, writePos, recLen);
            return UNKNOWN_ERROR;
        }
        inDisk[name] = writePos;
        writePos += recLen;
    }

    // 从缓存写一份（阶段B/C 共用）：返回 SUCCESS / FIND_FAILED(缓存无，跳过) / 错误码
    auto writeFromCache = [&](const std::string& name) -> int {
        Val val;
        {
            c->rwMutex->lock_shared();
            auto it = c->cache_db.find(name);
            if (it == c->cache_db.end()) {
                c->rwMutex->unlock_shared();
                return FIND_FAILED;
            }
            val = it->second;
            c->rwMutex->unlock_shared();
        }
        // 基本信息校验
        if (name.size() > NAME_LEN) return LONG_NAME;
        auto reg = typeReg.find(val.typeName);
        if (reg == typeReg.end()) return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）
        if (val.typeName.size() > TYPE_LEN) return LONG_NAME;

        // 组装（校验码直接 CHECK_VALID：阶段B 起旧区已无待读记录，本记录旧区不再需要读取）
        int dataSize = 0;
        int code = buildRecord(name, val, CHECK_VALID, dataSize);
        if (code != SUCCESS) return code;
        file.seekp(writePos);
        file.write(recBuf.data(), static_cast<std::streamsize>(recBuf.size()));
        if (!file) return UNKNOWN_ERROR;

        inDisk[name] = writePos;
        writePos += static_cast<long long>(recBuf.size());
        // 安全清脏：实体指针未变（期间未被 modData 替换）才清，防误清新脏数据
        {
            c->rwMutex->lock();
            auto it = c->cache_db.find(name);
            if (it != c->cache_db.end() && it->second.entity.get() == val.entity.get()) {
                it->second.isDirty = false;
            }
            c->rwMutex->unlock();
        }
        return SUCCESS;
    };

    /* ========== 阶段B：缓存脏 key（旧区已无待读记录，写多大都安全） ========== */
    for (const auto& [name, offset] : sorted) {
        if (!cacheDirty.contains(name)) continue;
        int code = writeFromCache(name);
        if (code == FIND_FAILED) {
            // 快照后已被淘汰：其脏落盘已排队到磁盘线程任务，压缩后由该任务追加，跳过即可
            continue;
        }
        if (code != SUCCESS) {
            reportRewriteFail(dbName, "cache", name, -1);
            return code;	// 组装/写入失败：重写中断（与既有语义一致，交运维）
        }
    }

    /* ========== 阶段C：缓存里 inDisk 没有的全新数据（从未落盘，必然脏） ========== */
    std::vector<std::string> newNames;
    {
        c->rwMutex->lock_shared();
        for (const auto& [name, val] : c->cache_db) {
            if (!inDisk.contains(name)) {
                newNames.emplace_back(name);
            }
        }
        c->rwMutex->unlock_shared();
    }
    for (const auto& name : newNames) {
        int code = writeFromCache(name);
        // FIND_FAILED = 收集后并发被删（淘汰落盘已排队），不写即可
        if (code != SUCCESS && code != FIND_FAILED) {
            reportRewriteFail(dbName, "cache-new", name, -1);
            return code;
        }
    }

    flushSync();
    if (!file) return UNKNOWN_ERROR;

    // 全部完成后截断到写游标位置（清掉空洞/已删数据），重写后 curSize 重新统计
    std::error_code ec;
    std::filesystem::resize_file(dbName, writePos, ec);
    if (ec) { reportRewriteFail(dbName, "resize", "", -1); return UNKNOWN_ERROR; }	// 截断失败

    curSize = writePos;
    clearHoles();	// 紧凑文件无洞：清空洞索引（此前登记的洞已被压缩回收）
    // —— 压缩观测记账（仅成功路径；失败由调用方 compactFail 计数）——
    statCompactUs.fetch_add(static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - compactT0).count()), std::memory_order_relaxed);
    statCompactBefore.fetch_add(compactBeforeBytes, std::memory_order_relaxed);
    statCompactAfter.fetch_add(writePos, std::memory_order_relaxed);
    return SUCCESS;
}
