#include "disk.h"
#include <cstring>
#include <fstream>


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
    long long createUS = std::chrono::duration_cast<std::chrono::microseconds>(val.createTime.time_since_epoch()).count();
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
    file.write((char*)&createUS, CREATE_TIME_LEN);
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

int Disk::persisData(std::list<std::string>& varNameSet)
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
        long long createUS = std::chrono::duration_cast<std::chrono::microseconds>(val.createTime.time_since_epoch()).count();
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
        file.write((char*)&createUS, CREATE_TIME_LEN);
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
    std::list<std::string> dirtyNames;
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
    return persisData(dirtyNames);
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
    long long createUS = 0, expireUS = 0;
    char isPermanent = 0;
    file.read((char*)&createUS, CREATE_TIME_LEN);
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
    val.createTime = std::chrono::system_clock::time_point(std::chrono::microseconds(createUS));
    val.isPermanent = isPermanent;	// char 转 bool：非 0 即 true
    val.expireTime = val.isPermanent ? val.createTime
        : std::chrono::system_clock::time_point(std::chrono::microseconds(expireUS));
    return SUCCESS;
}

int Disk::selData(std::list<std::string>& varNameSet, std::list<std::any>& resSet)
{
    return 0;
}

int Disk::flushDisk()
{
    return 0;
}

int Disk::reWrite() {
    return 0;
}
