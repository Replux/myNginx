
#ifndef __NGX_MEMORY_H__
#define __NGX_MEMORY_H__

#include <stddef.h>

//内存相关类
class CMemory 
{
private:
	CMemory() {}  //因为要做成单例类，所以将构造函数设为private

public:
	~CMemory(){};

private:
	static CMemory *instance;

public:	
	static CMemory* GetInstance() //单例
	{			
		//lock(由于本程序都是在master初始化单例类，故无需设置互斥锁，往后有需要再加)
		if(instance == NULL)
		{
			instance = new CMemory(); //第一次调用不应该放在线程中，应该放在主进程中，以免和其他线程调用冲突从而导致同时执行两次new CMemory()
			static FinalizeCMemory gc; 
		}
		//unlock
		return instance;
	}	
	class FinalizeCMemory 
	{
	public:				
		~FinalizeCMemory()
		{
			if (CMemory::instance)
			{						
				delete CMemory::instance;
				CMemory::instance = NULL;				
			}
		}
	};

public:
	void *AllocMemory(int memCount,bool ifmemset);
	void FreeMemory(void *point);
	
};

#endif
