#ifndef __NGX_C_CRC32_H__
#define __NGX_C_CRC32_H__

#include <stddef.h>

class CCRC32
{
private:
	CCRC32();
public:
	~CCRC32();
private:
	static CCRC32 *instance;

public:	
	static CCRC32* GetInstance() 
	{
		//lock(由于本程序都是在master初始化单例类，故无需设置互斥锁，往后有需要再加)
		if(instance == NULL)
		{
			instance = new CCRC32();
			static FinalizeCCRC32 gc; 
		}
		//unlock
		return instance;
	}	
	class FinalizeCCRC32
	{
	public:				
		~FinalizeCCRC32()
		{
			if (CCRC32::instance)
			{						
				delete CCRC32::instance;
				CCRC32::instance = NULL;				
			}
		}
	};
public:

	void  Init_CRC32_Table();
	//unsigned long Reflect(unsigned long ref, char ch); // Reflects CRC bits in the lookup table
    unsigned int Reflect(unsigned int ref, char ch); // Reflects CRC bits in the lookup table
    
	//int   Get_CRC(unsigned char* buffer, unsigned long dwSize);
    int   Get_CRC(unsigned char* buffer, unsigned int dwSize);
    
public:
	//unsigned long crc32_table[256]; // Lookup table arrays
    unsigned int crc32_table[256]; // Lookup table arrays
};

#endif


