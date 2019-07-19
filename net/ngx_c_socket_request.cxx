
//和网络  中 客户端发送来数据/服务器端收包 有关的代码

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
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"  //自动释放互斥量的一个类

//当连接上传入数据时候的时候，本函数会被ngx_epoll_process_events()所调用
void CSocket::ngx_read_request_handler(lpngx_connection_t pConn)
{  
    bool isflood = false;

    ssize_t reco = recvproc(pConn,pConn->precvbuf,pConn->irecvlen); 
    if(reco <= 0)
        return;

    if(pConn->curStat == _PKG_HD_INIT) //ngx_get_connection()在初始化连接时以将连接赋为_PKG_HD_INIT
    {        
        if(reco == sizeof(COMM_PKG_HEADER)) //收到完整包头，这里拆解包头
        {   
            MessageHandler_p1(pConn,isflood); 
        }
        else  //包头未接收完整
		{
			pConn->curStat   = _PKG_HD_RECVING;
            pConn->precvbuf  = pConn->precvbuf + reco;
            pConn->irecvlen  = pConn->irecvlen - reco;
        }
    } 
    else if(pConn->curStat == _PKG_HD_RECVING) //包头未接收完整，则继续接收
    {
        if(pConn->irecvlen == reco) 
        {
            MessageHandler_p1(pConn,isflood); 
        }
        else //包头还是没收完整，继续收包头
		{
            pConn->precvbuf  = pConn->precvbuf + reco;
            pConn->irecvlen  = pConn->irecvlen - reco;
        }
    }
    else if(pConn->curStat == _PKG_BD_INIT)  //包头刚好收完，准备接收包体
    {
        if(reco == pConn->irecvlen)
        {
            if(m_floodAkEnable == 1) 
            {
                isflood = TestFlood(pConn);
            }
            MessageHandler_p2(pConn,isflood);
        }
        else  //包体没收完整
		{
			pConn->curStat = _PKG_BD_RECVING;					
			pConn->precvbuf = pConn->precvbuf + reco;
			pConn->irecvlen = pConn->irecvlen - reco;
		}
    }
    else if(pConn->curStat == _PKG_BD_RECVING)  //包体没收完整，则继续接收
    {
        if(pConn->irecvlen == reco)
        {
            if(m_floodAkEnable == 1) 
            {
                isflood = TestFlood(pConn);
            }
            MessageHandler_p2(pConn,isflood);
        }
        else //包体还是没收完整，继续收
        {
            pConn->precvbuf = pConn->precvbuf + reco;
			pConn->irecvlen = pConn->irecvlen - reco;
        }
    }

    if(isflood == true)
        zdClosesocketProc(pConn);

    return;
}

//接收数据专用函数--引入这个函数是为了方便，如果断线，错误之类的，这里直接 释放连接池中连接，然后直接关闭socket，以免在其他函数中还要重复的干这些事
//参数c：连接池中相关连接
//参数buff：接收数据的缓冲区
//参数buflen：要接收的数据大小
//返回值：返回-1，则是有问题发生并且在这里把问题处理完毕了，调用本函数的调用者一般是可以直接return
//        返回>0，则是表示实际收到的字节数
ssize_t CSocket::recvproc(lpngx_connection_t pConn,char *buff,ssize_t buflen)  //ssize_t是有符号整型，在32位机器上等同与int，在64位机器上等同与long int，size_t就是无符号型的ssize_t
{
    ssize_t n;
    
    n = recv(pConn->fd, buff, buflen, 0); //recv()系统函数， 最后一个参数flag，一般为0；     
    if(n == 0) //客户端close()断连
    {
        zdClosesocketProc(pConn);        
        return -1;
    }

    if(n < 0) //说明有错误发生
    {
        if(errno == EAGAIN || errno == EWOULDBLOCK)
        {
            ngx_log_stderr(errno,"CSocket::recvproc()中errno == EAGAIN || errno == EWOULDBLOCK成立，出乎意料！");//epoll为LT模式不应该出现这个返回值，所以直接打印出来瞧瞧
            return -1;
        }
        if(errno == EINTR) 
        {
            ngx_log_stderr(errno,"CSocket::recvproc()中errno == EINTR成立，出乎意料！");//epoll为LT模式不应该出现这个返回值，所以直接打印出来瞧瞧
            return -1;
        }

        if(errno == ECONNRESET)  //客户端发送RST，粗暴断连
        {
            // ...待扩展
        }
        else if(errno == EBADF)  // Bad file descriptor，并发环境下，socket可能会被干掉
        {
            // ...待扩展
        } 
        else
        {
            ngx_log_stderr(errno,"CSocket::recvproc()中发生错误");
        }

        zdClosesocketProc(pConn);
        return -1;
    }

    //能走到这里的，就认为收到了有效数据

    return n; //返回收到的字节数
}


//消息收完整后的处理，称为消息处理阶段1【p1】
void CSocket::MessageHandler_p1(lpngx_connection_t pConn,bool &isflood)
{    
    CMemory *p_memory = CMemory::GetInstance();		

    LPCOMM_PKG_HEADER pPkgHeader;
    pPkgHeader = (LPCOMM_PKG_HEADER)pConn->dataHeadInfo;

    unsigned short e_pkgLen; 
    e_pkgLen = ntohs(pPkgHeader->pkgLen);
    
    if(e_pkgLen < sizeof(COMM_PKG_HEADER))  //报文总长度 < 包头长度，认定非法包，状态和接收位置都复原
    {
        pConn->curStat = _PKG_HD_INIT;      
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = sizeof(COMM_PKG_HEADER);
    }
    else if(e_pkgLen > (PKG_MAX_LENGTH-1000))   //包的总长度大于限制则判定为恶意包，状态和接收位置都复原
    {
        pConn->curStat = _PKG_HD_INIT;
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = sizeof(COMM_PKG_HEADER);
    }
    else //合法的包头，继续处理
    {
        char *pTmpBuffer  = (char *)p_memory->AllocMemory(sizeof(STRUC_MSG_HEADER) + e_pkgLen,false); //分配内存【长度是 消息头+包头+包体】       
        pConn->precvMemPointer = pTmpBuffer;  //指向接收数据的首地址

        //a)先填写消息头内容
        LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)pTmpBuffer;
        ptmpMsgHeader->pConn = pConn;
        ptmpMsgHeader->iCurrsequence = pConn->iCurrsequence; //收到包时的连接池中连接序号记录到消息头里来
        //b)再填写包头内容
        pTmpBuffer += sizeof(STRUC_MSG_HEADER); 
        memcpy(pTmpBuffer,pPkgHeader,sizeof(COMM_PKG_HEADER));
        if(e_pkgLen == sizeof(COMM_PKG_HEADER)) //该报文只有包头无包体
        {
            if(m_floodAkEnable == 1) 
            {
                isflood = TestFlood(pConn);
            }
            MessageHandler_p2(pConn,isflood);
        } 
        else
        {
            pConn->curStat = _PKG_BD_INIT;
            pConn->precvbuf = pTmpBuffer + sizeof(COMM_PKG_HEADER);
            pConn->irecvlen = e_pkgLen - sizeof(COMM_PKG_HEADER);
        }                       
    }  //end if(e_pkgLen < sizeof(COMM_PKG_HEADER)) 

    return;
}

//消息body接收完整后的处理，称为消息处理阶段2【p2】
//注意参数isflood是个引用
void CSocket::MessageHandler_p2(lpngx_connection_t pConn,bool &isflood)
{
    if(isflood == false)
    {
        g_threadpool.inMsgRecvQueueAndSignal(pConn->precvMemPointer); //入消息队列并触发线程处理消息
    }
    else
    {
        CMemory *p_memory = CMemory::GetInstance();
        p_memory->FreeMemory(pConn->precvMemPointer); //直接释放掉内存，根本不往消息队列入
    }

    pConn->precvMemPointer = NULL;
    pConn->curStat         = _PKG_HD_INIT;     //收包状态机的状态恢复为原始态，为收下一个包做准备                    
    pConn->precvbuf        = pConn->dataHeadInfo;  //设置好收包的位置
    pConn->irecvlen        = sizeof(COMM_PKG_HEADER);  //设置好要接收数据的大小
    return;
}

//测试是否flood攻击成立，成立则返回true，否则返回false
bool CSocket::TestFlood(lpngx_connection_t pConn)
{
    struct  timeval sCurrTime;   //当前时间结构
	uint64_t        iCurrTime;   //当前时间（单位：毫秒）
	bool  reco      = false;
	
	gettimeofday(&sCurrTime, NULL); //取得当前时间
    iCurrTime =  (sCurrTime.tv_sec * 1000 + sCurrTime.tv_usec / 1000);  //毫秒
	if((iCurrTime - pConn->FloodkickLastTime) < m_floodTimeInterval)   //两次收到包的时间 < 100毫秒
	{
		pConn->FloodAttackCount++; 
		pConn->FloodkickLastTime = iCurrTime;
	}
	else //恢复计数值
	{
		pConn->FloodAttackCount = 0;
		pConn->FloodkickLastTime = iCurrTime;
	}

	if(pConn->FloodAttackCount >= m_floodKickCount) 
		reco = true; //可以踢此人的标志
        
	return reco;
}

//发送数据专用函数，返回本次发送的字节数
//返回 > 0，成功发送了一些字节
//=0，估计对方断了
//-1，errno == EAGAIN ，本方发送缓冲区满了
//-2，errno != EAGAIN != EWOULDBLOCK != EINTR
ssize_t CSocket::sendproc(lpngx_connection_t c,char *buff,ssize_t size)  //ssize_t是有符号整型，在32位机器上等同与int，在64位机器上等同与long int，size_t就是无符号型的ssize_t
{
    ssize_t   n;

    for ( ;; )
    {
        n = send(c->fd, buff, size, 0);

        if(n > 0)
            return n;
        else if(n == 0)
            return 0;
            
        if(errno == EAGAIN)
            return -1;

        if(errno == EINTR)
            ngx_log_stderr(errno,"CSocket::sendproc()中send()失败.");      
        else
            return -2;

    }
}

//能走到这里，数据就是没法送完毕，要继续发送
void CSocket::ngx_write_request_handler(lpngx_connection_t pConn)
{      
    CMemory *p_memory = CMemory::GetInstance();
    
    ssize_t sendsize = sendproc(pConn,pConn->psendbuf,pConn->isendlen);

    if(sendsize > 0 && sendsize != pConn->isendlen)
    {        
        //没有全部发送完毕，数据只发出去了一部分，那么发送到了哪里，剩余多少，继续记录，方便下次sendproc()时使用
        pConn->psendbuf = pConn->psendbuf + sendsize;
		pConn->isendlen = pConn->isendlen - sendsize;	
        return;
    }
    else if(sendsize == -1)
    {
        ngx_log_stderr(errno,"CSocket::ngx_write_request_handler()时if(sendsize == -1)成立，这很怪异。"); //打印个日志，别的先不干啥
        return;
    }

    if(sendsize > 0 && sendsize == pConn->isendlen) //成功发送完毕，做个通知是可以的；
    {
        //如果成功发送完毕数据，则把写事件通知从epoll中移除；其他情况就是断线，等着系统内核把连接从红黑树中删除即可；
        if(ngx_epoll_oper_event(
                pConn->fd,          //socket句柄
                EPOLL_CTL_MOD,      //事件类型，这里是修改
                EPOLLOUT,           //标志，这里代表要减去的标志,EPOLLOUT：可写
                1,                  //对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数, 0：增加   1：去掉 2：完全覆盖
                pConn               //连接池中的连接
                ) == -1)
        {
            ngx_log_stderr(errno,"CSocket::ngx_write_request_handler()中ngx_epoll_oper_event()失败。");
        }    
    }

    //能走下来的，要么数据发送完毕了，要么对端断开了，那么执行收尾工作吧；

    p_memory->FreeMemory(pConn->psendMemPointer);
    pConn->psendMemPointer = NULL;        
    --pConn->iThrowsendCount;

    if(sem_post(&m_semEventSendQueue)==-1)       
        ngx_log_stderr(0,"CSocket::ngx_write_request_handler()中sem_post(&m_semEventSendQueue)失败.");
    return;
}

//消息处理线程主函数，专门处理各种接收到的TCP消息
//pMsgBuf：发送过来的消息缓冲区，消息本身是自解释的，通过包头可以计算整个包长
//         消息本身格式【消息头+包头+包体】 
void CSocket::threadRecvProcFunc(char *pMsgBuf)
{   
    return;
}


