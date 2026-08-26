#include "cache.h"


Cache::Cache(const long long memoSize, const int batchSize, const int upDisEdge, const int minDisEdge)
{
	this->memoSize = memoSize;
	this->curSize = 0;
	this->rwMutex = std::make_unique<WritePrefMutex>(batchSize, upDisEdge, minDisEdge);
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
