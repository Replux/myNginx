//和处理系统配置文件相关的放这里

//系统头文件放上边
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

//自定义头文件放下边,因为g++中用了-I参数，所以这里用<>也可以
#include "ngx_func.h"     //函数声明
#include "ngx_c_conf.h"   //和配置文件处理相关的类,名字带c_表示和类有关

//静态成员赋值
CConfig *CConfig::m_instance = NULL;

//构造函数
CConfig::CConfig()
{		
}

//析构函数
CConfig::~CConfig()
{    
	std::vector<LPCConfItem>::iterator pos;	
	for(pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
	{		
		delete (*pos);
	}
	m_ConfigItemList.clear(); 
    return;
}

//装载配置文件
bool CConfig::Load(const char *pconfName) 
{   
    FILE *fp;
    fp = fopen(pconfName,"r");
    if(fp == NULL)
        return false;

    //每一行配置文件读出来都放这里
    char  linebuf[501];   //每行配置都不要太长，保持<500字符内，防止出现问题
    
    //走到这里，文件打开成功 
    while(!feof(fp))  //检查文件是否结束 ，没有结束则条件成立
    {    
        if(fgets(linebuf,500,fp) == NULL) //从文件中读数据，每次读一行，一行最多不要超过500个字符 
            continue;

        if(linebuf[0] == 0)
            continue;

        //处理注释行
        if(*linebuf==';' || *linebuf==' ' || *linebuf=='#' || *linebuf=='\t'|| *linebuf=='\n')
			continue;
        
    lblprocstring:
        //屁股后边若有换行，回车，空格等都截取掉
		if(strlen(linebuf) > 0)
		{
			if(linebuf[strlen(linebuf)-1] == 10 || linebuf[strlen(linebuf)-1] == 13 || linebuf[strlen(linebuf)-1] == 32) 
			{
				linebuf[strlen(linebuf)-1] = 0;
				goto lblprocstring;
			}		
		}
        if(linebuf[0] == 0)
            continue;
        if(*linebuf=='[') //[开头的也不处理
			continue;

        //语法满足要求的语句才会执行至此处
        char *ptmp = strchr(linebuf,'=');
        if(ptmp != NULL)
        {
            LPCConfItem p_confitem = new CConfItem;                    //注意前边类型带LP，后边new这里的类型不带
            memset(p_confitem,0,sizeof(CConfItem));
            strncpy(p_confitem->ItemName,linebuf,(int)(ptmp-linebuf)); //等号左侧的拷贝到p_confitem->ItemName
            strcpy(p_confitem->ItemContent,ptmp+1);                    //等号右侧的拷贝到p_confitem->ItemContent

            Rtrim(p_confitem->ItemName);
			Ltrim(p_confitem->ItemName);
			Rtrim(p_confitem->ItemContent);
			Ltrim(p_confitem->ItemContent);
        
            m_ConfigItemList.push_back(p_confitem);  //在m_ConfigItemList的析构函数中统一释放
        }
    } //end while(!feof(fp)) 

    fclose(fp);
    return true;
}

//根据ItemName获取配置信息字符串
const char *CConfig::GetString(const char *p_itemname)
{
	std::vector<LPCConfItem>::iterator pos;	
	for(pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
	{	
		if(strcasecmp( (*pos)->ItemName,p_itemname) == 0)
			return (*pos)->ItemContent;
	}
	return NULL;
}
//根据ItemName获取数字类型配置信息
int CConfig::GetIntDefault(const char *p_itemname,const int def)
{
	std::vector<LPCConfItem>::iterator pos;	
	for(pos = m_ConfigItemList.begin(); pos !=m_ConfigItemList.end(); ++pos)
	{	
		if(strcasecmp( (*pos)->ItemName,p_itemname) == 0)
			return atoi((*pos)->ItemContent);
	}
	return def;
}



