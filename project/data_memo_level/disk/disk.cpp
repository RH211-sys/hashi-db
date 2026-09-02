#include "disk.h"
#include "../cache/cache.h"	// 方法实现需访问 Cache 完整类型（友元 + 私有成员）
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

/*
    <fstream> 用于读写文件内容
    <filesystem> 用于操作文件和目录本身。
*/


Disk::Disk(const long long& maxSize, std::string& dbName)
{
    this->curSize = 0;
    this->dbName = dbName;
    this->maxSize = maxSize;

}

void Disk::setCache(std::shared_ptr<Cache> c)
{
    cache = c;
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

    /* ========== 序列化 =========== */

    std::vector<char> bytes = reg->second.first(val.entity);	// 实体序列化字节
    int dataSize = (int)bytes.size();			// 数据大小：只含实体
    int recLen = CODE_LEN + ENTITY_SIZE_LEN + TIME_INFO_LEN + TYPE_LEN + NAME_LEN + dataSize;	// 记录总长：校验码 + 实体大小 + 时间信息 + 类型名定长 + 变量名定长 + 实体

    // 时间信息：微秒整数（8B），与 std::chrono 互转
    long long updateUS = std::chrono::duration_cast<std::chrono::microseconds>(val.updateTime.time_since_epoch()).count();
    long long expireUS = 
        val.isPermanent ? 0 : std::chrono::duration_cast<std::chrono::microseconds>(val.expireTime.time_since_epoch()).count();

    // 类型名补 '\0' 到定长
    char typeBuf[TYPE_LEN] = { 0 };
    memcpy(typeBuf, val.typeName.c_str(), val.typeName.size());

    // 变量名补 '\0' 到定长
    char nameBuf[NAME_LEN] = { 0 };
    memcpy(nameBuf, varName.c_str(), varName.size());

    /* ========== 打开文件并进行写入 =========== */

    // 确保文件存在（in|out 模式打不开不存在的文件，首次写先创建）
    std::ofstream touch(dbName, std::ios::binary | std::ios::app);
    touch.close();

    std::fstream file(dbName, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) return FILE_OPEN_FILED;	// 文件打开失败

    // 写文件前先清 isDirty（写锁内）：防止写文件期间缓存被改（新脏数据）后误清
    // 若写文件失败则置回 true，等待下次重刷
    {
        c->rwMutex->lock();
        auto it = c->cache_db.find(varName);
        if (it != c->cache_db.end()) {
            it->second.isDirty = false;
        }
        c->rwMutex->unlock();
    }

    // 一律追加写：旧记录无法保证长度一致，覆盖会产生碎片，留空洞等重写回收
    file.seekp(0, std::ios::end);
    int offset = (int)file.tellp();	// 记录偏移 = 文件当前大小

    // 两段式写：先写无效校验码整条，写完回写有效码（写一半崩溃 → 校验码无效，重建时截断）
    file.seekp(offset);
    char broken = CHECK_BROKEN;
    file.write(&broken, CODE_LEN);
    file.write((char*)&dataSize, ENTITY_SIZE_LEN);
    file.write((char*)&updateUS, UPDATE_TIME_LEN);
    file.write((char*)&expireUS, EXPIRE_TIME_LEN);
    file.write((char*)&val.isPermanent, IS_PERMANENT_LEN);
    file.write(typeBuf, TYPE_LEN);
    file.write(nameBuf, NAME_LEN);
    file.write(bytes.data(), dataSize);
    file.seekp(offset);		// 回写有效校验码
    char valid = CHECK_VALID;
    file.write(&valid, CODE_LEN);
    file.flush();
    if (!file) {
        // 写失败：isDirty 置回 true，等待下次重刷
        {
            c->rwMutex->lock();
            auto it = c->cache_db.find(varName);
            if (it != c->cache_db.end()) {
                it->second.isDirty = true;
            }
            c->rwMutex->unlock();
        }
        return UNKNOWN_ERROR;	// 写入失败
    }

    /* ========== 更新disk的大小记录 =========== */

    // 同名变量可能有多条记录（旧版本），inDisk 指向最新一条；重建时同名覆盖，旧记录成空洞
    inDisk[varName] = offset;
    curSize += recLen;	// 只增不减：空洞/已删数据不回收，重写时重新统计
    return SUCCESS;
}

int Disk::persisData(const std::string& varName, const Val& val)
{
    // 与无数据版流程一致，但数据由调用方提供：不查缓存、不动 isDirty（淘汰时缓存已删）

    /* ========== 检查基本信息是否正确 =========== */

    // 变量名超长：定长字段放不下，无法写入
    if (varName.size() > NAME_LEN) return LONG_NAME;

    // 查类型注册表，序列化实体（any 类型擦除，运行时只能按 typeName 查表拿模板实例）
    auto reg = typeReg.find(val.typeName);
    if (reg == typeReg.end()) return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）

    // 类型名超长：定长字段放不下，截断后无法反序列化
    if (val.typeName.size() > TYPE_LEN) return LONG_NAME;

    /* ========== 序列化 =========== */

    std::vector<char> bytes = reg->second.first(val.entity);	// 实体序列化字节
    int dataSize = (int)bytes.size();			// 数据大小：只含实体
    int recLen = CODE_LEN + ENTITY_SIZE_LEN + TIME_INFO_LEN + TYPE_LEN + NAME_LEN + dataSize;	// 记录总长：校验码 + 实体大小 + 时间信息 + 类型名定长 + 变量名定长 + 实体

    // 时间信息：微秒整数（8B），与 std::chrono 互转
    long long updateUS = std::chrono::duration_cast<std::chrono::microseconds>(val.updateTime.time_since_epoch()).count();
    long long expireUS =
        val.isPermanent ? 0 : std::chrono::duration_cast<std::chrono::microseconds>(val.expireTime.time_since_epoch()).count();

    // 类型名补 '\0' 到定长
    char typeBuf[TYPE_LEN] = { 0 };
    memcpy(typeBuf, val.typeName.c_str(), val.typeName.size());

    // 变量名补 '\0' 到定长
    char nameBuf[NAME_LEN] = { 0 };
    memcpy(nameBuf, varName.c_str(), varName.size());

    /* ========== 打开文件并进行写入 =========== */

    // 确保文件存在（in|out 模式打不开不存在的文件，首次写先创建）
    std::ofstream touch(dbName, std::ios::binary | std::ios::app);
    touch.close();

    std::fstream file(dbName, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) return FILE_OPEN_FILED;	// 文件打开失败

    // 一律追加写：旧记录无法保证长度一致，覆盖会产生碎片，留空洞等重写回收
    file.seekp(0, std::ios::end);
    int offset = (int)file.tellp();	// 记录偏移 = 文件当前大小

    // 两段式写：先写无效校验码整条，写完回写有效码（写一半崩溃 → 校验码无效，重建时截断）
    file.seekp(offset);
    char broken = CHECK_BROKEN;
    file.write(&broken, CODE_LEN);
    file.write((char*)&dataSize, ENTITY_SIZE_LEN);
    file.write((char*)&updateUS, UPDATE_TIME_LEN);
    file.write((char*)&expireUS, EXPIRE_TIME_LEN);
    file.write((char*)&val.isPermanent, IS_PERMANENT_LEN);
    file.write(typeBuf, TYPE_LEN);
    file.write(nameBuf, NAME_LEN);
    file.write(bytes.data(), dataSize);
    file.seekp(offset);		// 回写有效校验码
    char valid = CHECK_VALID;
    file.write(&valid, CODE_LEN);
    file.flush();
    if (!file) {
        return UNKNOWN_ERROR;	// 写入失败（数据已从缓存删，无法重试，调用方自行处理）
    }

    /* ========== 更新disk的大小记录 =========== */

    // 同名变量可能有多条记录（旧版本），inDisk 指向最新一条；重建时同名覆盖，旧记录成空洞
    inDisk[varName] = offset;
    curSize += recLen;	// 只增不减：空洞/已删数据不回收，重写时重新统计
    return SUCCESS;
}

int Disk::persisData(std::vector<std::string> varNameSet)
{
    if (varNameSet.empty()) return SUCCESS;

    // 从缓存拿缓存指针（非锁，不阻塞）
    auto c = cache.lock();
    if (!c) return UNKNOWN_ERROR;	// 缓存未绑定或已析构

    // 确保文件存在（in|out 模式打不开不存在的文件，首次写先创建）
    std::ofstream touch(dbName, std::ios::binary | std::ios::app);
    touch.close();

    // 一次文件开关写入多条数据：批量落盘，减少物理磁盘 IO 频率
    // （写入先进页缓存，多个数据可能合并为一次物理落盘）
    std::fstream file(dbName, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) return FILE_OPEN_FILED;	// 文件打开失败

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
        // 查类型注册表，序列化实体（any 类型擦除，运行时只能按 typeName 查表拿模板实例）
        auto reg = typeReg.find(val.typeName);
        if (reg == typeReg.end()) return TYPE_VALID;	// 类型未注册（忘了 DEFINE_DATA_TYPE）
        // 类型名超长：定长字段放不下，截断后无法反序列化
        if (val.typeName.size() > TYPE_LEN) return LONG_NAME;

        /* ========== 序列化 =========== */
        std::vector<char> bytes = reg->second.first(val.entity);	// 实体序列化字节
        int dataSize = (int)bytes.size();			// 数据大小：只含实体
        int recLen = CODE_LEN + ENTITY_SIZE_LEN + TIME_INFO_LEN + TYPE_LEN + NAME_LEN + dataSize;	// 记录总长：校验码 + 实体大小 + 时间信息 + 类型名定长 + 变量名定长 + 实体

        // 时间信息：微秒整数（8B），与 std::chrono 互转
        long long updateUS = std::chrono::duration_cast<std::chrono::microseconds>(val.updateTime.time_since_epoch()).count();
        long long expireUS = val.isPermanent ? 0
            : std::chrono::duration_cast<std::chrono::microseconds>(val.expireTime.time_since_epoch()).count();

        // 类型名补 '\0' 到定长
        char typeBuf[TYPE_LEN] = { 0 };
        memcpy(typeBuf, val.typeName.c_str(), val.typeName.size());

        // 变量名补 '\0' 到定长
        char nameBuf[NAME_LEN] = { 0 };
        memcpy(nameBuf, varName.c_str(), varName.size());

        /* ========== 追加写入 =========== */
        // 写文件前先清 isDirty（写锁内）：防止写文件期间缓存被改（新脏数据）后误清
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

        // 一律追加写：旧记录无法保证长度一致，覆盖会产生碎片，留空洞等重写回收
        file.seekp(0, std::ios::end);
        int offset = (int)file.tellp();	// 记录偏移 = 文件当前大小

        // 两段式写：先写无效校验码整条，写完回写有效码（写一半崩溃 → 校验码无效，重建时截断）
        file.seekp(offset);
        char broken = CHECK_BROKEN;
        file.write(&broken, CODE_LEN);
        file.write((char*)&dataSize, ENTITY_SIZE_LEN);
        file.write((char*)&updateUS, UPDATE_TIME_LEN);
        file.write((char*)&expireUS, EXPIRE_TIME_LEN);
        file.write((char*)&val.isPermanent, IS_PERMANENT_LEN);
        file.write(typeBuf, TYPE_LEN);
        file.write(nameBuf, NAME_LEN);
        file.write(bytes.data(), dataSize);
        file.seekp(offset);		// 回写有效校验码
        char valid = CHECK_VALID;
        file.write(&valid, CODE_LEN);

        /* ========== 更新disk的大小记录 =========== */
        // 同名变量可能有多条记录（旧版本），inDisk 指向最新一条；重建时同名覆盖，旧记录成空洞
        inDisk[varName] = offset;
        curSize += recLen;	// 只增不减：空洞/已删数据不回收，重写时重新统计
        return SUCCESS;
    };

    /* ========== 循环写入 =========== */
    int firstError = SUCCESS;	// 记录首个错误码，其余变量继续处理
    for (const auto& varName : varNameSet) {
        int code = writeOne(varName);
        if (code != SUCCESS && firstError == SUCCESS) {
            firstError = code;
        }
    }

    file.flush();
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
    val.updateTime = std::chrono::system_clock::time_point(std::chrono::microseconds(updateUS));
    val.isPermanent = isPermanent;	// char 转 bool：非 0 即 true
    val.expireTime = val.isPermanent ? val.updateTime
        : std::chrono::system_clock::time_point(std::chrono::microseconds(expireUS));
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
		val.updateTime = std::chrono::system_clock::time_point(std::chrono::microseconds(updateUS));
		val.isPermanent = isPermanent;	// char 转 bool：非 0 即 true
		val.expireTime = val.isPermanent ? val.updateTime
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
                inDisk.erase(it->first);	// 磁盘索引删：重写主循环不再搬移它
                it = c->cache_db.erase(it);
            } else {
                ++it;
            }
        }
        c->rwMutex->unlock();
    }

    // 确保文件存在（in|out 模式打不开不存在的文件）
    std::ofstream touch(dbName, std::ios::binary | std::ios::app);
    touch.close();

    std::fstream file(dbName, std::ios::in | std::ios::out | std::ios::binary);
    if (!file) return FILE_OPEN_FILED;	// 文件打开失败

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

        /* ========== 序列化 =========== */
        std::vector<char> bytes = reg->second.first(val.entity);	// 实体序列化字节
        int dataSize = (int)bytes.size();
        int recLen = CODE_LEN + ENTITY_SIZE_LEN + TIME_INFO_LEN + TYPE_LEN + NAME_LEN + dataSize;

        // 时间信息：微秒整数（8B），与 std::chrono 互转
        long long updateUS = std::chrono::duration_cast<std::chrono::microseconds>(val.updateTime.time_since_epoch()).count();
        long long expireUS = val.isPermanent ? 0
            : std::chrono::duration_cast<std::chrono::microseconds>(val.expireTime.time_since_epoch()).count();

        char typeBuf[TYPE_LEN] = { 0 };
        memcpy(typeBuf, val.typeName.c_str(), val.typeName.size());
        char nameBuf[NAME_LEN] = { 0 };
        memcpy(nameBuf, name.c_str(), name.size());

        // 先清 isDirty（写锁内，与持久化一致：写文件期间缓存被改会重新置脏，不被误清）
        {
            c->rwMutex->lock();
            auto it = c->cache_db.find(name);
            if (it != c->cache_db.end()) {
                it->second.isDirty = false;
            }
            c->rwMutex->unlock();
        }

        /* ========== 写到紧凑位置（原地重写，不是追加） =========== */
        file.seekp(writePos);
        char valid = CHECK_VALID;
        file.write(&valid, CODE_LEN);
        file.write((char*)&dataSize, ENTITY_SIZE_LEN);
        file.write((char*)&updateUS, UPDATE_TIME_LEN);
        file.write((char*)&expireUS, EXPIRE_TIME_LEN);
        file.write((char*)&val.isPermanent, IS_PERMANENT_LEN);
        file.write(typeBuf, TYPE_LEN);
        file.write(nameBuf, NAME_LEN);
        file.write(bytes.data(), dataSize);
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
        writePos += recLen;
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

    file.flush();
    if (!file) return UNKNOWN_ERROR;

    // 全部完成后截断到写游标位置（清掉空洞/已删数据），重写后 curSize 重新统计
    std::error_code ec;
    std::filesystem::resize_file(dbName, writePos, ec);
    if (ec) return UNKNOWN_ERROR;	// 截断失败

    curSize = writePos;
    return SUCCESS;
}
