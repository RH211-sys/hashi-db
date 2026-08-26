#include "disk.h"

Disk::Disk(const long long& maxSize, std::string& dbName)
{
    this->curSize = 0;
    this->dbName = dbName;
    this->maxSize = maxSize;
}

void Disk::setCache(Cache* c)
{
}


int Disk::delData(const std::string& varName)
{
    return 0;
}


int Disk::persisData(const std::string& varName)
{
    return 0;
}

int Disk::persisData(std::list<std::string>& varNameSet)
{
    return 0;
}

int Disk::selData(const std::string& varName, std::any& res)
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
