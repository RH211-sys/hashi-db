#include "disk.h"
#include "../cache/cache.h"	// 方法实现需访问 Cache 完整类型（友元 + 私有成员）
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    inDisk[varName] = static_cast<int>(offset);
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
    return s;
}


int Disk::delData(const std::string& varName)
{
    // 数据仍在文件里（inDisk 保留偏移），只标记逻辑删除，重写时跳过清理
    if (inDisk.contains(varName)) {
        inDisk.erase(varName);
        return SUCCESS;
    }
    // 该数据不存在，可能已经被删除了
    return FIND_FAILED;
}

int Disk::delData(std::vector<std::string> varNameSet)
{
    for (const auto& varName : varNameSet) {
        inDisk.erase(varName);	// 不存在则无操作，不视为错误
    }
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
        if (code != SUCCESS && firstError == SUCCESS) {
            firstError = code;
        }
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
    /* ========== 查内存索引 =========== */
    auto it = inDisk.find(varName);
    if (it == inDisk.end()) return FIND_FAILED;	// 磁盘没有该数据
    int offset = it->second;

    // 读耗时观测起点（含文件开关 + 读记录 + 反序列化，成功读回才记账）
    auto t0 = std::chrono::steady_clock::now();

    /* ========== 打开文件读取 =========== */
    std::ifstream file(dbName, std::ios::binary);
    if (!file) return FILE_OPEN_FILED;	// 文件打开失败

    /* ========== 读记录头 =========== */
    file.seekg(offset);
    char check = 0;
    file.read(&check, CODE_LEN);
    if (!file || check != CHECK_VALID) return FIND_FAILED;	// 校验码无效：数据不完整

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
    if (memcmp(nameBuf, varName.c_str(), varName.size()) != 0) return FIND_FAILED;

    /* ========== 读实体并反序列化 =========== */
    std::vector<char> bytes(dataSize);
    file.read(bytes.data(), dataSize);
    if (!file) return FIND_FAILED;	// 读取失败（数据不完整）

    // 查类型注册表，反序列化实体（any 类型擦除，按 typeName 查表拿模板实例）
    std::string typeName(typeBuf);	// 截到 '\0'
    auto reg = typeReg.find(typeName);
    if (reg == typeReg.end()) return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）
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
        1. 过期数据：缓存删除 + 磁盘索引删除（防止从磁盘复活，磁盘记录留空洞等重写回收）
        2. 未过期脏数据：收集变量名，锁外批量刷盘，防止锁堵住
    */
    std::vector<std::string> flushNames;
    {
        c->rwMutex->lock();
        auto now = std::chrono::system_clock::now();    

        auto it = c->cache_db.begin();
        while (it != c->cache_db.end()) {
            auto& val = it->second;
            if (!val.isPermanent && val.expireTime <= now) {
                // 过期：直接删除（缓存权威，过期数据不落盘）
                c->curSize -= val.dataSize;	// 更新缓存当前大小
                inDisk.erase(it->first);
                it = c->cache_db.erase(it);
            } else {
                if (val.isDirty) flushNames.emplace_back(it->first);	// 未过期脏数据：收集刷盘
                ++it;
            }
        }
        c->rwMutex->unlock();
    }
    /*
        复用persisData重载（一次文件开关，写前清 isDirty，失败统一置回）
        inDisk 中已存在的数据：追加写后偏移覆盖为最新（逻辑覆盖），旧记录成空洞等重写回收
    */
    return persisData(std::move(flushNames));
}

int Disk::reWrite()
{
    // 重写源：inDisk（磁盘已有数据）+ 缓存全部数据（缓存中 inDisk 没有的全新数据也要写入）
    // 从缓存拿缓存指针（cache是weak_ptr类型）
    auto c = cache.lock();
    if (!c) return UNKNOWN_ERROR;	// 缓存未绑定或已析构

    // 重写 = 整理所有数据：先删掉过期缓存数据（缓存删 + 磁盘索引删，重写时跳过，不留文件防复活）
    {
        c->rwMutex->lock();	// 有删除操作，需要写锁
        auto now = std::chrono::system_clock::now();
        auto it = c->cache_db.begin();
        while (it != c->cache_db.end()) {
            auto& val = it->second;
            if (!val.isPermanent && val.expireTime <= now) {
                c->curSize -= val.dataSize;	// 更新缓存当前大小
                inDisk.erase(it->first);	// 磁盘索引删：重写主循环不再搬移它
                it = c->cache_db.erase(it);
            } else {
                ++it;
            }
        }
        c->rwMutex->unlock();
    }

    // 复用常驻句柄（惰性打开/异常重建见 ensureFileOpen），本函数内读写与截断都走同一句柄
    if (!ensureFileOpen()) return FILE_OPEN_FILED;	// 文件打开失败
    std::fstream& file = this->file;

    // 写游标，写永远不覆盖未重写记录
    int writePos = 0;

    /*
        把缓存数据序列化写到紧凑位置（写游标），更新 inDisk/写游标
        缓存没有该数据返回 FIND_FAILED（调用方走磁盘搬移）
    */
    auto writeCacheData = [&](const std::string& name) -> int {
        /* ========== 检查缓存是否存在该数据 =========== */
        Val val;
        {
            c->rwMutex->lock_shared();	// 读锁内拷贝 Val，锁外使用
            auto it = c->cache_db.find(name);
            if (it == c->cache_db.end()) {
                c->rwMutex->unlock_shared();	// 缓存没有：先解锁再返回，防止读锁泄漏
                return FIND_FAILED;
            }
            val = it->second;
            c->rwMutex->unlock_shared();
        }

        /* ========== 检查基本信息是否正确 =========== */
        if (name.size() > NAME_LEN) return LONG_NAME;
        auto reg = typeReg.find(val.typeName);
        if (reg == typeReg.end()) return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）
        if (val.typeName.size() > TYPE_LEN) return LONG_NAME;

        /* ========== 组装 + 写到紧凑位置（原地重写，不是追加） =========== */

        // 先清 isDirty（写锁内，与持久化一致：写文件期间缓存被改会重新置脏，不被误清）
        {
            c->rwMutex->lock();
            auto it = c->cache_db.find(name);
            if (it != c->cache_db.end()) {
                it->second.isDirty = false;
            }
            c->rwMutex->unlock();
        }

        // 整条记录组装进 recBuf（定长头 + 实体直写缓冲，校验码直接置 CHECK_VALID——原地重写无需两段式）
        int dataSize = 0;
        int code = buildRecord(name, val, CHECK_VALID, dataSize);
        if (code != SUCCESS) {
            // 组装失败：isDirty 置回 true，等待下次重刷
            c->rwMutex->lock();
            auto it = c->cache_db.find(name);
            if (it != c->cache_db.end()) {
                it->second.isDirty = true;
            }
            c->rwMutex->unlock();
            return code;
        }

        // 整条一次写（不覆盖未重写记录：写游标只前进）
        file.seekp(writePos);
        file.write(recBuf.data(), static_cast<std::streamsize>(recBuf.size()));
        if (!file) {
            c->rwMutex->lock();
            auto it = c->cache_db.find(name);
            if (it != c->cache_db.end()) {
                it->second.isDirty = true;
            }
            c->rwMutex->unlock();
            return UNKNOWN_ERROR;	// 写入失败：重写中断，文件半新半旧，数据全损，交运维（事先备份）
        }
        // 偏移更新为紧凑位置，写游标前进
        inDisk[name] = writePos;
        writePos += static_cast<int>(recBuf.size());
        return SUCCESS;
    };

    // 对 inDisk 的偏移量排序（临时 vector 拷贝 + sort，重写低频，不必常驻有序结构），从偏移量小的开始遍历
    /* 可能可以优化? */
    std::vector<std::pair<std::string, int>> sorted(inDisk.begin(), inDisk.end());
    std::sort(sorted.begin(), sorted.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    for (const auto& [name, offset] : sorted) {
        // 缓存中存在 → 直接持久化缓存数据 (脏数据以缓存为主( 缓存写回 ))，写完后清 isDirty
        int code = writeCacheData(name);
        if (code == FIND_FAILED) {
            /* ========== 不在缓存 → 从磁盘整条搬到紧凑位置（读一条 → 写一条） =========== */
            // inDisk 的偏移必然是完整记录（校验码 VALID），校验码原样搬移
            file.seekg(offset);
            char check = 0;
            int dataSize = 0;
            file.read(&check, CODE_LEN);
            file.read((char*)&dataSize, ENTITY_SIZE_LEN);
            if (!file || check != CHECK_VALID) return UNKNOWN_ERROR;	// 记录异常：数据不完整

            int recLen = CODE_LEN + ENTITY_SIZE_LEN + TIME_INFO_LEN + TYPE_LEN + NAME_LEN + dataSize;
            std::vector<char> buf(recLen);
            file.seekg(offset);
            file.read(buf.data(), recLen);
            if (!file) return UNKNOWN_ERROR;

            file.seekp(writePos);
            file.write(buf.data(), recLen);
            if (!file) return UNKNOWN_ERROR;

            // 偏移更新为紧凑位置，写游标前进
            inDisk[name] = writePos;
            writePos += recLen;
        }
        else if (code != SUCCESS) {
            return code;	// 检查/序列化/写入失败
        }
    }

    // 缓存中 inDisk 没有的数据（全新未刷盘）：也要写入文件（读锁内收集，锁外写入）
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
        int code = writeCacheData(name);
        // FIND_FAILED = 收集后并发被删（数据已不存在，不写即可），其余错误返回
        if (code != SUCCESS && code != FIND_FAILED) return code;
    }

    flushSync();
    if (!file) return UNKNOWN_ERROR;

    // 全部完成后截断到写游标位置（清掉空洞/已删数据），重写后 curSize 重新统计
    std::error_code ec;
    std::filesystem::resize_file(dbName, writePos, ec);
    if (ec) return UNKNOWN_ERROR;	// 截断失败

    curSize = writePos;
    return SUCCESS;
}
