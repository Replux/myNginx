#include <stdarg.h>
#include <unistd.h>  //usleep

#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_threadpool.h"
#include "ngx_c_memory.h"
#include "ngx_macro.h"

//静态成员初始化
pthread_mutex_t CThreadPool::m_threadMutex = PTHREAD_MUTEX_INITIALIZER;  //#define PTHREAD_MUTEX_INITIALIZER ((pthread_mutex_t) -1)
pthread_cond_t CThreadPool::m_pthreadCond = PTHREAD_COND_INITIALIZER;     //#define PTHREAD_COND_INITIALIZER ((pthread_cond_t) -1)
bool CThreadPool::m_shutdown = false;    //刚开始标记整个线程池的线程是不退出的      

//构造函数
CThreadPool::CThreadPool()
{
    m_iRunningThreadNum = 0;  //正在运行的线程，开始给个0【注意这种写法：原子的对象给0也可以直接赋值，当整型变量来用】
    m_iLastEmgTime = 0;       //上次报告线程不够用了的时间；
    m_iRecvMsgQueueCount = 0; //收消息队列
}

//析构函数
CThreadPool::~CThreadPool()
{    
    //接收消息队列中内容释放
    clearMsgRecvQueue();
}

//各种清理函数
//清理接收消息队列，注意这个函数的写法。
void CThreadPool::clearMsgRecvQueue()
{
	char * sTmpMempoint;
	CMemory *p_memory = CMemory::GetInstance();

	//尾声阶段，需要互斥？该退的都退出了，该停止的都停止了，应该不需要退出了
	while(!m_MsgRecvQueue.empty())
	{
		sTmpMempoint = m_MsgRecvQueue.front();		
		m_MsgRecvQueue.pop_front(); 
		p_memory->FreeMemory(sTmpMempoint);
	}	
}

//创建线程池中的线程，要手工调用，不在构造函数里调用了
//返回值：所有线程都创建成功则返回true，出现错误则返回false
bool CThreadPool::Create(int threadNum)
{    
    ThreadItem *pNew;
    int err;
    m_iThreadNum = threadNum; //记录线程池中线程的数量  
    
    for(int i = 0; i < m_iThreadNum; ++i)
    {
        m_threadVector.push_back(pNew = new ThreadItem(this));       
        err = pthread_create(&pNew->_Handle, NULL, ThreadFunc, pNew);
        if(err != 0)
        {
            ngx_log_stderr(err,"CThreadPool::Create()创建线程%d失败，返回的错误码为%d!",i,err);
            return false;
        } 
    }

    //等待每个线程都启动并运行到pthread_cond_wait()，本函数才返回 
    std::vector<ThreadItem*>::iterator iter;

lblfor:
    for(iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        if( (*iter)->ifReady == false)
        {
            usleep(100 * 1000);  //单位是微妙,所以100 *1000 = 100毫秒
            goto lblfor;
        }
    }
    return true;
}

//线程工作函数
void* CThreadPool::ThreadFunc(void* threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CThreadPool *pThreadPoolObj = pThread->_pThis; //获取该线程所在的线程池
    CMemory *p_memory = CMemory::GetInstance();	    
    int err;
    pthread_t tid = pthread_self();

    while(true)
    { 
        err = pthread_mutex_lock(&m_threadMutex);  
        if(err != 0) ngx_log_stderr(err,"CThreadPool::ThreadFunc()中pthread_mutex_lock()失败，返回的错误码为%d!",err);//有问题，要及时报告
        
        //此处是为了解决虚假唤醒的问题，ref:https://blog.csdn.net/Replus_/article/details/88836149
        while ( (pThreadPoolObj->m_MsgRecvQueue.size() == 0) && m_shutdown == false)
        {
            if(pThread->ifReady == false)            
                pThread->ifReady = true; 
                
            //整个服务器程序刚初始化的时，让所有工作线程卡在这等着被唤醒
            pthread_cond_wait(&m_pthreadCond, &m_threadMutex); 
        }

        //能走下来的，线程满足两种情况：(1)拿到任务队列中的数据，(2)m_shutdown==true
        
        if(m_shutdown) //判断是否是第二种情况
        {   
            pthread_mutex_unlock(&m_threadMutex);
            break;                     
        }

        //走到这里，只有可能是第一种情况，将任务取出
        char *jobbuf = pThreadPoolObj->m_MsgRecvQueue.front();
        pThreadPoolObj->m_MsgRecvQueue.pop_front();
        --pThreadPoolObj->m_iRecvMsgQueueCount;
               
        err = pthread_mutex_unlock(&m_threadMutex); 
        if(err != 0)  ngx_log_stderr(err,"CThreadPool::ThreadFunc()中pthread_mutex_unlock()失败，返回的错误码为%d!",err);
        
        ++pThreadPoolObj->m_iRunningThreadNum;     //原子+1【正在干活的线程数量增加1】
        g_socket.threadRecvProcFunc(jobbuf);       //处理任务
        p_memory->FreeMemory(jobbuf);              //处理完以后，释放内存 
        --pThreadPoolObj->m_iRunningThreadNum;     //原子-1【正在干活的线程数量减少1】

    } //end while(true)

    return (void*)0;
}

//停止所有线程【等待结束线程池中所有线程，该函数返回后，应该是所有线程池中线程都结束了】
void CThreadPool::StopAll() 
{
    //(1)已经调用过，就不要重复调用了
    if(m_shutdown == true)
        return;

    m_shutdown = true;

    //(2)唤醒卡在pthread_cond_wait()中的所有线程
    int err = pthread_cond_broadcast(&m_pthreadCond); 
    if(err != 0)
    {
        ngx_log_stderr(err,"CThreadPool::StopAll()中pthread_cond_broadcast()失败，返回的错误码为%d!",err);
        return;
    }

    //(3)等等线程，让线程真返回    
    std::vector<ThreadItem*>::iterator iter;
	for(iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        pthread_join((*iter)->_Handle, NULL);
    }

    pthread_mutex_destroy(&m_threadMutex);
    pthread_cond_destroy(&m_pthreadCond);    

    //(4)释放线程池中的线程
	for(iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
	{
		if(*iter) delete *iter;
	}
	m_threadVector.clear();

    ngx_log_stderr(0,"CThreadPool::StopAll()成功返回，线程池中线程全部正常结束!");
    return;    
}


//收到一个完整消息后，入消息队列，并触发线程池中线程来处理该消息
void CThreadPool::inMsgRecvQueueAndSignal(char *buf)
{
    int err = pthread_mutex_lock(&m_threadMutex);    //lock 1
    if(err != 0)
        ngx_log_stderr(err,"CThreadPool::inMsgRecvQueueAndSignal()pthread_mutex_lock()失败，返回的错误码为%d!",err);

    m_MsgRecvQueue.push_back(buf);
    ++m_iRecvMsgQueueCount;
    err = pthread_mutex_unlock(&m_threadMutex);    //unlock 1

    if(err != 0)
        ngx_log_stderr(err,"CThreadPool::inMsgRecvQueueAndSignal()pthread_mutex_unlock()失败，返回的错误码为%d!",err);

    Call();                          
    return;
}

//来任务了，调一个线程池中的线程下来干活
void CThreadPool::Call()
{
    int err = pthread_cond_signal(&m_pthreadCond); //唤醒一个等待该条件的线程，也就是可以唤醒卡在pthread_cond_wait()的线程
    if(err != 0 )
    {
        ngx_log_stderr(err,"CThreadPool::Call()中pthread_cond_signal()失败，返回的错误码为%d!",err);
    }
    
    //(1)如果当前没有闲置的工作线程，则要报警
    if(m_iThreadNum == m_iRunningThreadNum)
    {        
        time_t currtime = time(NULL);
        if(currtime - m_iLastEmgTime > 10) //间隔10秒以上
        {
            m_iLastEmgTime = currtime;  //更新时间
            //写日志，通知这种紧急情况给用户，用户要考虑增加线程池中线程数量了
            ngx_log_stderr(0,"CThreadPool::Call()中发现线程池中当前空闲线程数量为0，要考虑扩容线程池了!");
        }
    } //end if 

    return;
}

//唤醒丢失问题，sem_t sem_write;
//参考信号量解决方案：https://blog.csdn.net/yusiguyuan/article/details/20215591  linux多线程编程--信号量和条件变量 唤醒丢失事件
