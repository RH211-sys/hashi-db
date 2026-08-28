#include "disk.h"


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

    return 0;
}

int Disk::persisData(std::list<std::string>& varNameSet)
{
    return 0;
}

int Disk::persisAll()
{
    return 0;
}

int Disk::selData(const std::string& varName, std::any& res, Val& val)
{
    return 0;
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

}
