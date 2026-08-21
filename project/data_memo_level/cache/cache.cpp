#include "cache.h"


Cache::Cache(long long memoSize)
{
	this->memoSize = memoSize;
	this->curSize = 0;
}

void Cache::setDisk(Disk* d)
{
}

int Cache::selData(const std::string& varName, std::any& res)
{
	return 0;
}

int Cache::persisVar(const std::string& varName)
{
	return 0;
}

int Cache::persisVar()
{
	return 0;
}

int Cache::reWrite()
{
	return 0;
}
