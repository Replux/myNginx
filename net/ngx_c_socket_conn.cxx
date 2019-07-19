
//和网络 中 连接/连接池 有关的函数放这里
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

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"


//连接池成员函数
ngx_connection_s::ngx_connection_s()//构造函数
{		
    iCurrsequence = 0;    
    pthread_mutex_init(&logicPorcMutex, NULL); //互斥量初始化
}
ngx_connection_s::~ngx_connection_s()//析构函数
{
    pthread_mutex_destroy(&logicPorcMutex);    //互斥量释放
}

//分配出去一个连接的时候初始化一些内容,
void ngx_connection_s::Recondition()
{
    ++iCurrsequence;

    fd  = -1;                                         //开始先给-1
    curStat = _PKG_HD_INIT;                           //收包状态处于 初始状态，准备接收数据包头【状态机】
    precvbuf = dataHeadInfo;                         
    irecvlen = sizeof(COMM_PKG_HEADER);               //这里指定收数据的长度，这里先要求收包头这么长字节的数据
    
    precvMemPointer   = NULL;                         //既然没new内存，那自然指向的内存地址先给NULL
    iThrowsendCount   = 0;                            //原子的
    psendMemPointer   = NULL;                         //发送数据头指针记录
    events            = 0;                            //epoll事件先给0 
    lastPingTime      = time(NULL);                   //上次ping的时间

    FloodkickLastTime = 0;                            //Flood攻击上次收到包的时间
	FloodAttackCount  = 0;	                          //Flood攻击在该时间内收到包的次数统计
    iSendCount        = 0;                            //发送队列中有的数据条目数，若client只发不收，则可能造成此数过大，依据此数做出踢出处理 
}


//初始化连接池
void CSocket::InitConnPool()
{
    lpngx_connection_t p_Conn;
    CMemory *p_memory = CMemory::GetInstance();   

    int ilenconnpool = sizeof(ngx_connection_t);    
    for(int i = 0; i < m_worker_connections; ++i) //先创建这么多个连接，后续不够再增加
    {
        p_Conn = (lpngx_connection_t)p_memory->AllocMemory(ilenconnpool,true); //清理内存 , 因为这里分配内存new char，无法执行构造函数，所以如下：
        //手工调用构造函数，因为AllocMemory里无法调用构造函数
        p_Conn = new(p_Conn) ngx_connection_t();  //定位new，释放则显式调用p_Conn->~ngx_connection_t();		
        p_Conn->Recondition();
        m_connectionList.push_back(p_Conn);     //所有连接【不管是否空闲】都放在这个list
        m_freeconnectionList.push_back(p_Conn); //空闲连接会放在这个list
    }
    m_free_connection_n = m_total_connection_n = m_connectionList.size(); //开始这两个列表一样大
    return;
}

//回收连接池，释放内存
void CSocket::ShutdownConnPool()
{
    lpngx_connection_t p_Conn;
	CMemory *p_memory = CMemory::GetInstance();
	
	while(!m_connectionList.empty())
	{
		p_Conn = m_connectionList.front();
		m_connectionList.pop_front(); 
        p_Conn->~ngx_connection_t();     //显式调用析构函数
		p_memory->FreeMemory(p_Conn);
	}
}

//从连接池中获取一个空闲连接
lpngx_connection_t CSocket::ngx_get_connection(int isock)
{
    CLock lock(&m_connectionMutex);  

    if(!m_freeconnectionList.empty()) //有空闲的，自然是从空闲的中摘取
    {
        
        lpngx_connection_t p_Conn = m_freeconnectionList.front();
        m_freeconnectionList.pop_front();
        p_Conn->Recondition();
        --m_free_connection_n; 
        p_Conn->fd = isock;
        return p_Conn;
    }

    //走到这里，表示没空闲的连接了，那就考虑重新创建一个连接
    CMemory *p_memory = CMemory::GetInstance();
    lpngx_connection_t p_Conn = (lpngx_connection_t)p_memory->AllocMemory(sizeof(ngx_connection_t),true);
    p_Conn = new(p_Conn) ngx_connection_t();
    p_Conn->Recondition();
    m_connectionList.push_back(p_Conn); //插入到总链表中来，但不能插入到空闲链表中
    ++m_total_connection_n;             
    p_Conn->fd = isock;
    return p_Conn;
}

//将连接还给连接池【立即回收】
void CSocket::ngx_free_connection(lpngx_connection_t pConn) 
{
    CLock lock(&m_connectionMutex);  

    ++pConn->iCurrsequence;

    if(pConn->precvMemPointer!=NULL)
    {
        CMemory::GetInstance()->FreeMemory(pConn->precvMemPointer);
        pConn->precvMemPointer = NULL; 
    }
    if(pConn->psendMemPointer != NULL) //如果发送数据的缓冲区里有内容，则要释放内存
    {
        CMemory::GetInstance()->FreeMemory(pConn->psendMemPointer);
        pConn->psendMemPointer = NULL;
    }
    pConn->iThrowsendCount = 0;

    m_freeconnectionList.push_back(pConn);

    ++m_free_connection_n;
    return;
}


//将连接放入回收队列(交付给回收线程处理)【延迟回收】
void CSocket::inRecyConnectQueue(lpngx_connection_t pConn)
{
    std::list<lpngx_connection_t>::iterator pos;
    bool ifFind = false;
    //针对连接回收队列的互斥量，因为线程ServerRecyConnectionThread()也有要用到这个回收队列
    CLock lock(&m_recyconnqueueMutex); 

    //看一下该连接是否已存在于回收队列中
    for(pos = m_recyconnectionList.begin(); pos != m_recyconnectionList.end(); ++pos)
	{
		if((*pos) == pConn)		
		{
			ifFind = true;
			return;
		}
	}

    pConn->inRecyTime = time(NULL);        //记录回收时间
    ++pConn->iCurrsequence;
    m_recyconnectionList.push_back(pConn); //等待ServerRecyConnectionThread线程自会处理 
    ++m_totol_recyconnection_n;            //待释放连接队列大小+1
    --m_onlineUserCount;                   //连入用户数量-1
    return;
}

//处理连接回收的线程
void* CSocket::ServerRecyConnectionThread(void* threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CSocket *pSocketObj = pThread->_pThis;
    
    time_t currtime;
    int err;
    std::list<lpngx_connection_t>::iterator pos,posend;
    lpngx_connection_t p_Conn;
    
    while(1)
    {
        usleep(500 * 1000); //每次休息500毫秒(单位是微妙,又因为1毫秒=1000微妙，所以 500 *1000 = 500毫秒)

        if(pSocketObj->m_totol_recyconnection_n > 0)
        {
            currtime = time(NULL);
            err = pthread_mutex_lock(&pSocketObj->m_recyconnqueueMutex);  
            if(err != 0) ngx_log_stderr(err,"CSocket::ServerRecyConnectionThread()中pthread_mutex_lock()失败，返回的错误码为%d!",err);

lblRRTD:
            pos    = pSocketObj->m_recyconnectionList.begin();
			posend = pSocketObj->m_recyconnectionList.end();
            for(; pos != posend; ++pos)
            {
                p_Conn = (*pos);
                if(
                    ( (p_Conn->inRecyTime + pSocketObj->m_conf_RecyConnWaitTime) > currtime)  && (g_stopEvent == 0)
                  )
                {
                    continue; //没到释放的时间
                }    

                //到释放的时间
                //凡是到释放时间的，iThrowsendCount都应该为0；这里再判断一下
                if(p_Conn->iThrowsendCount > 0)
                    ngx_log_stderr(0,"CSocket::ServerRecyConnectionThread()中到释放时间却发现p_Conn.iThrowsendCount!=0，这个不该发生");
                

                //流程走到这里，表示可以释放
                --pSocketObj->m_totol_recyconnection_n;        //待释放连接队列大小-1
                pSocketObj->m_recyconnectionList.erase(pos);   //迭代器已经失效，但pos所指内容在p_Conn里保存着呢

                pSocketObj->ngx_free_connection(p_Conn);	   //归还参数pConn所代表的连接到到连接池中
                goto lblRRTD; 
            }
            err = pthread_mutex_unlock(&pSocketObj->m_recyconnqueueMutex); 
            if(err != 0)  ngx_log_stderr(err,"CSocket::ServerRecyConnectionThread()pthread_mutex_unlock()失败，返回的错误码为%d!",err);
        } //end if(pSocketObj->m_totol_recyconnection_n > 0)

        if(g_stopEvent == 1) //要退出整个程序，那么肯定要先退出这个循环
        {
            if(pSocketObj->m_totol_recyconnection_n > 0)
            {
                //因为要退出，所以就得硬释放了【不管到没到时间，不管有没有其他不 允许释放的需求，都得硬释放】
                err = pthread_mutex_lock(&pSocketObj->m_recyconnqueueMutex);  
                if(err != 0) ngx_log_stderr(err,"CSocket::ServerRecyConnectionThread()中pthread_mutex_lock2()失败，返回的错误码为%d!",err);

        lblRRTD2:
                pos    = pSocketObj->m_recyconnectionList.begin();
			    posend = pSocketObj->m_recyconnectionList.end();
                for(; pos != posend; ++pos)
                {
                    p_Conn = (*pos);
                    --pSocketObj->m_totol_recyconnection_n;        //待释放连接队列大小-1
                    pSocketObj->m_recyconnectionList.erase(pos);   //迭代器已经失效，但pos所指内容在p_Conn里保存着呢
                    pSocketObj->ngx_free_connection(p_Conn);	   //归还参数pConn所代表的连接到到连接池中
                    goto lblRRTD2; 
                }
                err = pthread_mutex_unlock(&pSocketObj->m_recyconnqueueMutex); 
                if(err != 0)  ngx_log_stderr(err,"CSocket::ServerRecyConnectionThread()pthread_mutex_unlock2()失败，返回的错误码为%d!",err);
            }
            break; //整个程序要退出了，所以break;
        }
    } //end while(1) 
    
    return (void*)0;
}

void CSocket::ngx_close_connection(lpngx_connection_t pConn)
{    
    ngx_free_connection(pConn); 
    if(pConn->fd != -1)
    {
        close(pConn->fd);
        pConn->fd = -1;
    }    
    return;
}
