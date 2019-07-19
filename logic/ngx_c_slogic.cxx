
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
//#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
//#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_crc32.h"
#include "ngx_c_slogic.h"  
#include "ngx_logiccomm.h"  
#include "ngx_c_lockmutex.h"  

//定义成员函数指针
typedef bool (CLogicSocket::*handler)(  lpngx_connection_t pConn,       //连接池中连接的指针
                                        LPSTRUC_MSG_HEADER pMsgHeader,  //消息头指针
                                        char *pPkgBody,                 //包体指针
                                        unsigned short iBodyLength);    //包体长度

//用来保存 成员函数指针 的这么个数组
static const handler statusHandler[] = 
{
    //数组前5个元素，保留，以备将来增加一些基本服务器功能
    &CLogicSocket::_HandlePing,                             //【0】：心跳包的实现
    NULL,                                                   //【1】：下标从0开始
    NULL,                                                   //【2】：下标从0开始
    NULL,                                                   //【3】：下标从0开始
    NULL,                                                   //【4】：下标从0开始
    //开始处理具体的业务逻辑
    &CLogicSocket::_HandleRegister,                         //【5】：实现具体的注册功能
    &CLogicSocket::_HandleLogIn,                            //【6】：实现具体的登录功能
};

#define CMD_COUNT sizeof(statusHandler)/sizeof(handler) //整个命令有多少个，编译时即可知道

CLogicSocket::CLogicSocket() {}

CLogicSocket::~CLogicSocket() {}

//master执行
bool CLogicSocket::Initialize()
{     
    bool bParentInit = CSocket::Initialize();  //调用父类的同名函数
    return bParentInit;
}


//处理收到的数据包，由线程池来调用本函数，本函数是一个单独的线程；
//pMsgBuf：消息头 + 包头 + 包体 ：自解释；
void CLogicSocket::threadRecvProcFunc(char *pMsgBuf)
{
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;
    LPCOMM_PKG_HEADER  pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf+sizeof(STRUC_MSG_HEADER));
    unsigned short pkglen = ntohs(pPkgHeader->pkgLen);
    void  *pPkgBody;

    if(pkglen == sizeof(COMM_PKG_HEADER))
    {
		if(pPkgHeader->crc32 != 0) //只有包头的crc应该为0，若不为0说明该包异常，应当丢弃
			return;

		pPkgBody = NULL;
    }
    else  //有包体，走到这里
	{
		pPkgHeader->crc32 = ntohl(pPkgHeader->crc32);		          //针对4字节的数据，网络序转主机序
		pPkgBody = (void *)(pMsgBuf+sizeof(STRUC_MSG_HEADER)+sizeof(COMM_PKG_HEADER)); //跳过消息头和包头 ，指向包体
		//计算包体的crc值，以判断包的完整性        
		int calccrc = CCRC32::GetInstance()->Get_CRC((unsigned char *)pPkgBody,pkglen-sizeof(COMM_PKG_HEADER)); 
		if(calccrc != pPkgHeader->crc32)
		{
            ngx_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中CRC错误[服务器:%d/客户端:%d]，丢弃数据!",calccrc,pPkgHeader->crc32);    //正式代码中可以干掉这个信息
			return; //crc错，直接丢弃
		}      
	}

    //包crc校验无误    	
    unsigned short imsgCode = ntohs(pPkgHeader->msgCode); //取出消息类型
    lpngx_connection_t p_Conn = pMsgHeader->pConn;        //消息头中藏着指向连接池中连接的指针


    //(1)判断包是否过期
    if(p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)
        return; 

    //(2)检验消息类型是否在合理范围内
	if(imsgCode >= CMD_COUNT) //无符号数不可能<0
    {
        ngx_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码不对!",imsgCode);
        return;
    }

    //(3)有对应的消息处理函数吗
    if(statusHandler[imsgCode] == NULL)
    {
        ngx_log_stderr(0,"CLogicSocket::threadRecvProcFunc()中imsgCode=%d消息码找不到对应的处理函数!",imsgCode); //这种有恶意倾向或者错误倾向的包，希望打印出来看看是谁干的
        return;
    }

    //(4)一切正确，调用消息码对应的成员函数来处理
    (this->*statusHandler[imsgCode])(p_Conn,pMsgHeader,(char *)pPkgBody,pkglen-sizeof(COMM_PKG_HEADER));
    return;	
}

//心跳包检测时间到，该去检测心跳包是否超时的事宜，本函数是子类函数，实现具体的判断动作
void CLogicSocket::procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg,time_t cur_time)
{
    CMemory *p_memory = CMemory::GetInstance();
    lpngx_connection_t p_Conn = tmpmsg->pConn;

    if(tmpmsg->iCurrsequence == p_Conn->iCurrsequence)
    {
        if(m_ifTimeOutKick == 1)
        {
            zdClosesocketProc(p_Conn);
        }            
        else if( (cur_time - p_Conn->lastPingTime ) > (m_iWaitTime*3+10) ) //超时踢的判断标准就是 每次检查的时间间隔*3
        {          
            zdClosesocketProc(p_Conn); 
        }   
        p_memory->FreeMemory(tmpmsg);
    }
    else
    {
        p_memory->FreeMemory(tmpmsg);
    }
    return;
}


//发送没有包体的数据包给客户端
void CLogicSocket::SendNoBodyPkgToClient(LPSTRUC_MSG_HEADER pMsgHeader,unsigned short iMsgCode)
{
    CMemory  *p_memory = CMemory::GetInstance();

    char *p_sendbuf = (char *)p_memory->AllocMemory(sizeof(STRUC_MSG_HEADER)+sizeof(COMM_PKG_HEADER),false);
    char *p_tmpbuf = p_sendbuf;
    
	memcpy(p_tmpbuf,pMsgHeader,sizeof(STRUC_MSG_HEADER));
	p_tmpbuf += sizeof(STRUC_MSG_HEADER);

    LPCOMM_PKG_HEADER pPkgHeader = (LPCOMM_PKG_HEADER)p_tmpbuf;
    pPkgHeader->msgCode = htons(iMsgCode);	
    pPkgHeader->pkgLen = htons(sizeof(COMM_PKG_HEADER)); 
	pPkgHeader->crc32 = 0;		
    msgSend(p_sendbuf);
    return;
}

/* ____________________________________
 *
 *          处理各种业务逻辑
 * ____________________________________*/
bool CLogicSocket::_HandleRegister(lpngx_connection_t pConn,
                                LPSTRUC_MSG_HEADER pMsgHeader,
                                char *pPkgBody,
                                unsigned short iBodyLength)
{
    //(1)首先判断包体的合法性
    if(pPkgBody == NULL)    return false;
    if(iBodyLength != sizeof(STRUCT_REGISTER)) return false; 

    //(2)对于同一个用户，可能同时发送来多个请求过来，造成多个线程同时为该用户服务
    CLock lock(&pConn->logicPorcMutex); 
    
    //(3)取得了整个发送过来的数据
    LPSTRUCT_REGISTER p_RecvInfo = (LPSTRUCT_REGISTER)pPkgBody;
    p_RecvInfo->iType = ntohl(p_RecvInfo->iType);
    //以下两行非常关键，目的是防止客户端发来未带'\0'的畸形包，导致服务器非法访问自身内存。
    p_RecvInfo->username[sizeof(p_RecvInfo->username)-1]=0;
    p_RecvInfo->password[sizeof(p_RecvInfo->password)-1]=0; 

    //(4)给客户端返回数据时，一般也是返回一个结构，这个结构内容具体由客户端/服务器协商  
    LPCOMM_PKG_HEADER pPkgHeader;	
	CMemory  *p_memory = CMemory::GetInstance();
	CCRC32   *p_crc32 = CCRC32::GetInstance();
    //a)分配要发送出去的包的内存
    char *p_sendbuf = (char *)p_memory->AllocMemory(sizeof(STRUC_MSG_HEADER)+sizeof(COMM_PKG_HEADER)+sizeof(STRUCT_REGISTER),false);//准备发送的格式，这里是 消息头+包头+包体
    //b)填充消息头
    memcpy(p_sendbuf,pMsgHeader,sizeof(STRUC_MSG_HEADER));                   //消息头直接拷贝到这里来
    //c)填充包头
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf+sizeof(STRUC_MSG_HEADER));
    pPkgHeader->msgCode = htons(_CMD_REGISTER); //消息代码，可以统一在ngx_logiccomm.h中定义
    pPkgHeader->pkgLen  = htons(sizeof(COMM_PKG_HEADER) + sizeof(STRUCT_REGISTER)); 
    //d)填充包体
    LPSTRUCT_REGISTER p_sendInfo = (LPSTRUCT_REGISTER)(p_sendbuf+sizeof(STRUC_MSG_HEADER)+sizeof(COMM_PKG_HEADER));	//跳过消息头，跳过包头，就是包体了
    
    //e)包体内容全部确定好后，计算包体的crc32值
    pPkgHeader->crc32   = p_crc32->Get_CRC((unsigned char *)p_sendInfo,sizeof(STRUCT_REGISTER));
    pPkgHeader->crc32   = htonl(pPkgHeader->crc32);		

    //f)发送数据包
    msgSend(p_sendbuf);

    return true;
}

bool CLogicSocket::_HandleLogIn(lpngx_connection_t pConn,
                            LPSTRUC_MSG_HEADER pMsgHeader,
                            char *pPkgBody,
                            unsigned short iBodyLength)
{
    //(1)首先判断包体的合法性
    if(pPkgBody == NULL)  return false;
    if(iBodyLength != sizeof(STRUCT_LOGIN))  return false; 
    CLock lock(&pConn->logicPorcMutex);
        
    LPSTRUCT_LOGIN p_RecvInfo = (LPSTRUCT_LOGIN)pPkgBody;     
    p_RecvInfo->username[sizeof(p_RecvInfo->username)-1]=0;
    p_RecvInfo->password[sizeof(p_RecvInfo->password)-1]=0;

	LPCOMM_PKG_HEADER pPkgHeader;	
	CMemory  *p_memory = CMemory::GetInstance();
	CCRC32   *p_crc32 = CCRC32::GetInstance();

    int iSendLen = sizeof(STRUCT_LOGIN);  
    char *p_sendbuf = (char *)p_memory->AllocMemory(sizeof(STRUC_MSG_HEADER)+sizeof(COMM_PKG_HEADER)+iSendLen,false);    
    memcpy(p_sendbuf,pMsgHeader,sizeof(STRUC_MSG_HEADER));    
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf+sizeof(STRUC_MSG_HEADER));
    pPkgHeader->msgCode = _CMD_LOGIN;
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode);
    pPkgHeader->pkgLen  = htons(sizeof(COMM_PKG_HEADER) + iSendLen);    
    LPSTRUCT_LOGIN p_sendInfo = (LPSTRUCT_LOGIN)(p_sendbuf+sizeof(STRUC_MSG_HEADER)+sizeof(COMM_PKG_HEADER));
    pPkgHeader->crc32   = p_crc32->Get_CRC((unsigned char *)p_sendInfo,iSendLen);
    pPkgHeader->crc32   = htonl(pPkgHeader->crc32);
    msgSend(p_sendbuf);
    return true;
}

//接收并处理客户端发送过来的ping包
bool CLogicSocket::_HandlePing(lpngx_connection_t pConn,
                            LPSTRUC_MSG_HEADER pMsgHeader,
                            char *pPkgBody,
                            unsigned short iBodyLength)
{
    //心跳包要求没有包体；
    if(iBodyLength != 0)  //有包体则认为是 非法包
		return false; 

    CLock lock(&pConn->logicPorcMutex); //凡是和本用户有关的访问都考虑用互斥，以免该用户同时发送过来两个命令达到各种作弊目的
    pConn->lastPingTime = time(NULL);

    //服务器也返回一个只有包头的数据包给客户端
    SendNoBodyPkgToClient(pMsgHeader,_CMD_PING);

    return true;
}