﻿
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

//来数据时候的处理，当连接上有数据来的时候，本函数会被ngx_epoll_process_events()所调用  ,官方的类似函数为ngx_http_wait_request_handler();
void CSocket::ngx_read_request_handler(lpngx_connection_t pConn)
{  
    bool isflood = false; //是否flood攻击；

    ssize_t reco = recvproc(pConn,pConn->precvbuf,pConn->irecvlen); 
    if(reco <= 0)  
    {
        return;//该处理的上边这个recvproc()函数处理过了，这里<=0是直接return        
    }

    //走到这里，说明成功收到了一些字节（>0），就要开始判断收到了多少数据了     
    if(pConn->curStat == _PKG_HD_INIT) //连接建立起来时肯定是这个状态，因为在ngx_get_connection()中已经把curStat成员赋值成_PKG_HD_INIT了
    {        
        if(reco == sizeof(COMM_PKG_HEADER))//正好收到完整包头，这里拆解包头
        {   
            MessageHandler_p1(pConn,isflood); //那就调用专门针对包头处理完整的函数去处理把。
        }
        else
		{
			pConn->curStat        = _PKG_HD_RECVING;                 //接收包头中，包头不完整，继续接收包头中	
            pConn->precvbuf       = pConn->precvbuf + reco;              //注意收后续包的内存往后走
            pConn->irecvlen       = pConn->irecvlen - reco;              //要收的内容当然要减少，以确保只收到完整的包头先
        }
    } 
    else if(pConn->curStat == _PKG_HD_RECVING) //接收包头中，包头不完整，继续接收中，这个条件才会成立
    {
        if(pConn->irecvlen == reco) 
        {
            MessageHandler_p1(pConn,isflood); 
        }
        else
		{
			//包头还是没收完整，继续收包头
            //pConn->curStat        = _PKG_HD_RECVING;                 //没必要
            pConn->precvbuf       = pConn->precvbuf + reco;              //注意收后续包的内存往后走
            pConn->irecvlen       = pConn->irecvlen - reco;              //要收的内容当然要减少，以确保只收到完整的包头先
        }
    }
    else if(pConn->curStat == _PKG_BD_INIT)  //包头刚好收完，准备接收包体
    {
        if(reco == pConn->irecvlen) //收到的宽度等于要收的宽度，包体也收完整了
        {
            if(m_floodAkEnable == 1) 
            {
                isflood = TestFlood(pConn);
            }
            MessageHandler_p2(pConn,isflood);
        }
        else
		{
			//收到的宽度小于要收的宽度
			pConn->curStat = _PKG_BD_RECVING;					
			pConn->precvbuf = pConn->precvbuf + reco;
			pConn->irecvlen = pConn->irecvlen - reco;
		}
    }
    else if(pConn->curStat == _PKG_BD_RECVING)  //接收包体中，包体不完整，继续接收中
    {
        if(pConn->irecvlen == reco) //包体收完整了
        {
            if(m_floodAkEnable == 1) 
            {
                isflood = TestFlood(pConn);
            }
            MessageHandler_p2(pConn,isflood);
        }
        else
        {
            //包体没收完整，继续收
            pConn->precvbuf = pConn->precvbuf + reco;
			pConn->irecvlen = pConn->irecvlen - reco;
        }
    }  //end if(c->curStat == _PKG_HD_INIT)

    if(isflood == true)
    {
        //客户端flood服务器，则直接把客户端踢掉
        zdClosesocketProc(pConn);
    }

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
    if(n == 0)
    {
        zdClosesocketProc(pConn);        
        return -1;
    }
    //客户端没断，走这里 
    if(n < 0) //这被认为有错误发生
    {
        //EAGAIN和EWOULDBLOCK[【这个应该常用在hp上】应该是一样的值，表示没收到数据，一般来讲，在ET模式下会出现这个错误，因为ET模式下是不停的recv肯定有一个时刻收到这个errno，但LT模式下一般是来事件才收，所以不该出现这个返回值
        if(errno == EAGAIN || errno == EWOULDBLOCK)
        {
            ngx_log_stderr(errno,"CSocket::recvproc()中errno == EAGAIN || errno == EWOULDBLOCK成立，出乎意料！");//epoll为LT模式不应该出现这个返回值，所以直接打印出来瞧瞧
            return -1; //不当做错误处理，只是简单返回
        }
        //EINTR错误的产生：当阻塞于某个慢系统调用的一个进程捕获某个信号且相应信号处理函数返回时，该系统调用可能返回一个EINTR错误。
        //例如：在socket服务器端，设置了信号捕获机制，有子进程，当在父进程阻塞于慢系统调用时由父进程捕获到了一个有效信号时，内核会致使accept返回一个EINTR错误(被中断的系统调用)。
        if(errno == EINTR) 
        {
            ngx_log_stderr(errno,"CSocket::recvproc()中errno == EINTR成立，出乎意料！");//epoll为LT模式不应该出现这个返回值，所以直接打印出来瞧瞧
            return -1; //不当做错误处理，只是简单返回
        }

        //errno参考：http://dhfapiran1.360drm.com        
        if(errno == ECONNRESET)  //#define ECONNRESET 104 /* Connection reset by peer */
        {
            
        }
        else
        {
            if(errno == EBADF)  // #define EBADF   9 /* Bad file descriptor */
            {
                //因为多线程，偶尔会干掉socket，所以不排除产生这个错误的可能性
            }
            else
            {
                ngx_log_stderr(errno,"CSocket::recvproc()中发生错误");
            }
        } 
        
        zdClosesocketProc(pConn);
        return -1;
    }

    //能走到这里的，就认为收到了有效数据
    return n; //返回收到的字节数
}


//消息收完整后的处理，称为消息处理阶段1【p1】
//注意参数isflood是个引用
void CSocket::MessageHandler_p1(lpngx_connection_t pConn,bool &isflood)
{    
    CMemory *p_memory = CMemory::GetInstance();		

    LPCOMM_PKG_HEADER pPkgHeader;
    pPkgHeader = (LPCOMM_PKG_HEADER)pConn->dataHeadInfo; //正好收到包头时，包头信息肯定是在dataHeadInfo里；

    unsigned short e_pkgLen; 
    e_pkgLen = ntohs(pPkgHeader->pkgLen);
    //恶意包或者错误包的判断
    if(e_pkgLen < sizeof(COMM_PKG_HEADER)) 
    {
        //伪造包/或者包错误，否则整个包长怎么可能比包头还小？（整个包长是包头+包体，就算包体为0字节，那么至少e_pkgLen == sizeof(COMM_PKG_HEADER)）
        //报文总长度 < 包头长度，认定非法用户，废包
        //状态和接收位置都复原，这些值都有必要，因为有可能在其他状态比如_PKG_HD_RECVING状态调用这个函数；
        pConn->curStat = _PKG_HD_INIT;      
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = sizeof(COMM_PKG_HEADER);
    }
    else if(e_pkgLen > (PKG_MAX_LENGTH-1000))   //客户端发来包居然说包长度 > 29000?肯定是恶意包
    {
        //恶意包，太大，认定非法用户，废包【包头中说这个包总长度这么大，这不行】
        //状态和接收位置都复原，这些值都有必要，因为有可能在其他状态比如_PKG_HD_RECVING状态调用这个函数；
        pConn->curStat = _PKG_HD_INIT;
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = sizeof(COMM_PKG_HEADER);
    }
    else
    {
        //合法的包头，继续处理
        char *pTmpBuffer  = (char *)p_memory->AllocMemory(sizeof(STRUC_MSG_HEADER) + e_pkgLen,false); //分配内存【长度是 消息头+包头+包体】       
        pConn->precvMemPointer = pTmpBuffer;  //指向接收数据的首地址

        //a)先填写消息头内容
        LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)pTmpBuffer;
        ptmpMsgHeader->pConn = pConn;
        ptmpMsgHeader->iCurrsequence = pConn->iCurrsequence; //收到包时的连接池中连接序号记录到消息头里来，以备将来用；
        //b)再填写包头内容
        pTmpBuffer += sizeof(STRUC_MSG_HEADER);                 //往后跳，跳过消息头，指向包头
        memcpy(pTmpBuffer,pPkgHeader,sizeof(COMM_PKG_HEADER)); //直接把收到的包头拷贝进来
        if(e_pkgLen == sizeof(COMM_PKG_HEADER))
        {
            //该报文只有包头无包体
            //这相当于收完整了，则直接入消息队列待后续业务逻辑线程去处理吧
            if(m_floodAkEnable == 1) 
            {
                isflood = TestFlood(pConn);
            }
            MessageHandler_p2(pConn,isflood);
        } 
        else
        {
            pConn->curStat = _PKG_BD_INIT;                   //当前状态发生改变，包头刚好收完，准备接收包体	    
            pConn->precvbuf = pTmpBuffer + sizeof(COMM_PKG_HEADER);  //pTmpBuffer指向包头，这里 + sizeof(COMM_PKG_HEADER)后指向包体 weizhi
            pConn->irecvlen = e_pkgLen - sizeof(COMM_PKG_HEADER);    //e_pkgLen是整个包【包头+包体】大小，-sizeof(COMM_PKG_HEADER)【包头】  = 包体
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
        //对于有攻击倾向的恶人，先把他的包丢掉
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
        //发包太频繁记录
		pConn->FloodAttackCount++;
		pConn->FloodkickLastTime = iCurrTime;
	}
	else
	{
        //既然发布不这么频繁，则恢复计数值
		pConn->FloodAttackCount = 0;
		pConn->FloodkickLastTime = iCurrTime;
	}

	if(pConn->FloodAttackCount >= m_floodKickCount)
	{
		//可以踢此人的标志
		reco = true;
	}
	return reco;
}

//发送数据专用函数，返回本次发送的字节数
//返回 > 0，成功发送了一些字节
//=0，估计对方断了
//-1，errno == EAGAIN ，本方发送缓冲区满了
//-2，errno != EAGAIN != EWOULDBLOCK != EINTR
ssize_t CSocket::sendproc(lpngx_connection_t c,char *buff,ssize_t size)  //ssize_t是有符号整型，在32位机器上等同与int，在64位机器上等同与long int，size_t就是无符号型的ssize_t
{
    //这里参考借鉴了官方nginx函数ngx_unix_send()的写法
    ssize_t   n;

    for ( ;; )
    {
        n = send(c->fd, buff, size, 0); //send()系统函数， 最后一个参数flag，一般为0； 
        if(n > 0) //成功发送了一些数据
        {        
            //这里有两种情况
            //(1) n == size也就是想发送多少都发送成功了，这表示完全发完毕了
            //(2) n < size 没发送完毕，那肯定是发送缓冲区满了，所以也不必要重试发送，直接返回吧
            return n; //返回本次发送的字节数
        }

        if(n == 0)
        {
            return 0;
        }

        if(errno == EAGAIN)  //这东西应该等于EWOULDBLOCK
        {
            //内核缓冲区满，这个不算错误
            return -1;  //表示发送缓冲区满了
        }

        if(errno == EINTR) 
        {
            //这个应该也不算错误 ，收到某个信号导致send产生这个错误？
            //参考官方的写法，打印个日志，其他啥也没干，那就是等下次for循环重新send试一次了
            ngx_log_stderr(errno,"CSocket::sendproc()中send()失败.");  //打印个日志看看啥时候出这个错误          
        }
        else
        {
            return -2;    
        }
    } //end for
}

//能走到这里，数据就是没法送完毕，要继续发送
void CSocket::ngx_write_request_handler(lpngx_connection_t pConn)
{      
    CMemory *p_memory = CMemory::GetInstance();
    
    //这些代码的书写可以参照 void* CSocket::ServerSendQueueThread(void* threadData)
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
        //如果是成功的发送完毕数据，则把写事件通知从epoll中干掉吧；其他情况，那就是断线了，等着系统内核把连接从红黑树中干掉即可；
        if(ngx_epoll_oper_event(
                pConn->fd,          //socket句柄
                EPOLL_CTL_MOD,      //事件类型，这里是修改
                EPOLLOUT,           //标志，这里代表要减去的标志,EPOLLOUT：可写
                1,                  //对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数, 0：增加   1：去掉 2：完全覆盖
                pConn               //连接池中的连接
                ) == -1)
        {
            //有这情况发生？这可比较麻烦，不过先do nothing
            ngx_log_stderr(errno,"CSocket::ngx_write_request_handler()中ngx_epoll_oper_event()失败。");
        }    

        //ngx_log_stderr(0,"CSocket::ngx_write_request_handler()中数据发送完毕，很好。"); //做个提示吧，商用时可以干掉
        
    }

    //能走下来的，要么数据发送完毕了，要么对端断开了，那么执行收尾工作吧；

    p_memory->FreeMemory(pConn->psendMemPointer);  //释放内存
    pConn->psendMemPointer = NULL;        
    --pConn->iThrowsendCount;  //建议放在最后执行

    //数据发送完毕，或者把需要发送的数据干掉，都说明发送缓冲区可能有地方了，让发送线程往下走判断能否发送新数据
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


