
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
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include<typeinfo>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"


//构造函数
CSocket::CSocket()
{
    //配置相关
    m_worker_connections = 1;      //epoll连接最大项数
    m_ListenPortCount = 1;         //监听一个端口
    m_conf_RecyConnWaitTime = 60; //等待这么些秒后才回收连接

    //epoll相关
    m_epollhandle = -1;          //epoll返回的句柄 

    //各种队列相关
    m_iSendMsgQueueCount     = 0;     //发消息队列大小
    m_totol_recyconnection_n = 0;     //待释放连接队列大小
    m_cur_size_              = 0;     //当前计时队列尺寸
    m_earliestTime           = 0;     //当前计时队列头部的时间值
    m_iDiscardSendPkgCount   = 0;     //丢弃的发送数据包数量

    //在线用户相关
    m_onlineUserCount        = 0;     //在线用户数量统计，先给0  
    m_lastprintTime          = 0;     //上次打印统计信息的时间，先给0
    return;	
}

//初始化函数，master执行
//成功返回true，失败返回false
bool CSocket::Initialize()
{
    //__________读取配置文件，初始化成员变量__________
    CConfig *p_config = CConfig::GetInstance();
    m_worker_connections      = p_config->GetIntDefault("worker_connections",m_worker_connections);              //epoll连接的最大项数
    m_ListenPortCount         = p_config->GetIntDefault("ListenPortCount",m_ListenPortCount);                    //取得要监听的端口数量
    m_conf_RecyConnWaitTime  = p_config->GetIntDefault("Sock_RecyConnectionWaitTime",m_conf_RecyConnWaitTime); //等待这么些秒后才回收连接

    m_ifkickTimeCount         = p_config->GetIntDefault("Sock_WaitTimeEnable",0);                                //是否开启踢人时钟，1：开启   0：不开启
	m_iWaitTime               = p_config->GetIntDefault("Sock_MaxWaitTime",m_iWaitTime);                         //多少秒检测一次是否 心跳超时，只有当Sock_WaitTimeEnable = 1时，本项才有用	
	m_iWaitTime               = (m_iWaitTime > 5)?m_iWaitTime:5;                                                 //不建议低于5秒钟，因为无需太频繁
    m_ifTimeOutKick           = p_config->GetIntDefault("Sock_TimeOutKick",0);                                   //当时间到达Sock_MaxWaitTime指定的时间时，直接把客户端踢出去，只有当Sock_WaitTimeEnable = 1时，本项才有用 

    m_floodAkEnable          = p_config->GetIntDefault("Sock_FloodAttackKickEnable",0);                          //Flood攻击检测是否开启,1：开启   0：不开启
	m_floodTimeInterval      = p_config->GetIntDefault("Sock_FloodTimeInterval",100);                            //表示每次收到数据包的时间间隔是100(毫秒)
	m_floodKickCount         = p_config->GetIntDefault("Sock_FloodKickCounter",10);                              //累积多少次踢出此人

    return OpenListeningSocket();
}

/**
 * worker的初始化函数
 * ___________________________
 * 1.初始化线程在工作中需要用到的互斥量
 * 2.初始化信号量
 * 3.创建了回收连接、发送数据与检测心跳(可选)的工作线程，各一个
 * ______________________________________________________
 */
bool CSocket::Initialize_Worker()
{
    //发消息相关mutex初始化
    if(pthread_mutex_init(&m_sendMessageQueueMutex, NULL)  != 0)
    {        
        ngx_log_stderr(0,"CSocket::Initialize_Worker()中pthread_mutex_init(&m_sendMessageQueueMutex)失败.");
        return false;    
    }
    //连接相关mutex初始化
    if(pthread_mutex_init(&m_connectionMutex, NULL)  != 0)
    {
        ngx_log_stderr(0,"CSocket::Initialize_Worker()中pthread_mutex_init(&m_connectionMutex)失败.");
        return false;    
    }    
    //连接回收队列相关mutex初始化
    if(pthread_mutex_init(&m_recyconnqueueMutex, NULL)  != 0)
    {
        ngx_log_stderr(0,"CSocket::Initialize_Worker()中pthread_mutex_init(&m_recyconnqueueMutex)失败.");
        return false;    
    } 
    //时间处理队列有关的mutex初始化
    if(pthread_mutex_init(&m_timequeueMutex, NULL)  != 0)
    {
        ngx_log_stderr(0,"CSocket::Initialize_Worker()中pthread_mutex_init(&m_timequeueMutex)失败.");
        return false;    
    }
   
    //para2=0，表示信号量在线程之间共享。如果非0，表示在进程之间共享
    //para3=0，表示信号量的初始值，为0时，调用sem_wait()就会卡在那里卡着
    if(sem_init(&m_semEventSendQueue,0,0) == -1)
    {
        ngx_log_stderr(0,"CSocket::Initialize_Worker()中sem_init(&m_semEventSendQueue,0,0)失败.");
        return false;
    }

    //创建线程
    int err;
    ThreadItem *pSendQueue;    //专门用来发送数据的线程
    m_threadVector.push_back(pSendQueue = new ThreadItem(this)); //此处的this指向一个CLogicSocket的实例
    err = pthread_create(&pSendQueue->_Handle, NULL, ServerSendQueueThread,pSendQueue);
    if(err != 0)
    {
        ngx_log_stderr(0,"CSocket::Initialize_Worker()中pthread_create(ServerSendQueueThread)失败.");
        return false;
    }

    ThreadItem *pRecyconn;    //专门用来回收连接的线程
    m_threadVector.push_back(pRecyconn = new ThreadItem(this)); 
    err = pthread_create(&pRecyconn->_Handle, NULL, ServerRecyConnectionThread,pRecyconn);
    if(err != 0)
    {
        ngx_log_stderr(0,"CSocket::Initialize_Worker()中pthread_create(ServerRecyConnectionThread)失败.");
        return false;
    }

    if(m_ifkickTimeCount == 1)  //是否开启踢人时钟，1：开启   0：不开启
    {
        ThreadItem *pTimemonitor;    //专门用来处理到期不发心跳包的用户踢出的线程
        m_threadVector.push_back(pTimemonitor = new ThreadItem(this)); 
        err = pthread_create(&pTimemonitor->_Handle, NULL, ServerTimerQueueMonitorThread,pTimemonitor);
        if(err != 0)
        {
            ngx_log_stderr(0,"CSocket::Initialize_Worker()中pthread_create(ServerTimerQueueMonitorThread)失败.");
            return false;
        }
    }

    return true;
}


/**
 * 析构函数
 * ___________________________
 * 1.清空监听队列
 * 2.释放连接池资源
 * ______________________________________________________
 */
CSocket::~CSocket()
{
    //监听端口相关内存的释放
    std::vector<lpngx_listening_t>::iterator pos;

	for(pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
		delete (*pos);

	m_ListenSocketList.clear();    
    return;
}

//关闭退出函数(worker使用)
void CSocket::Shutdown_Worker()
{
    //(1)标记关闭记号
    g_stopEvent = 1;
    //(2)回收线程
    if(sem_post(&m_semEventSendQueue)==-1)
    {
        ngx_log_stderr(0,"CSocket::Shutdown_Worker()中sem_post(&m_semEventSendQueue)失败.");
    }

    std::vector<ThreadItem*>::iterator iter;
	for(iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        pthread_join((*iter)->_Handle, NULL); //等待线程终止
    }   
	for(iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
	{
		if(*iter)
			delete *iter;
	}
	m_threadVector.clear();

    //(3)清空队列
    clearMsgSendQueue();
    ShutdownConnPool();
    clearAllFromTimerQueue();
    
    //(4)销毁mutex和sem   
    pthread_mutex_destroy(&m_connectionMutex);          //连接相关互斥量释放
    pthread_mutex_destroy(&m_sendMessageQueueMutex);    //发消息互斥量释放    
    pthread_mutex_destroy(&m_recyconnqueueMutex);       //连接回收队列相关的互斥量释放
    pthread_mutex_destroy(&m_timequeueMutex);           //时间处理队列相关的互斥量释放
    sem_destroy(&m_semEventSendQueue);                  //发消息相关线程信号量释放
}

//清理TCP发送消息队列
void CSocket::clearMsgSendQueue()
{
	char * sTmpMempoint;
	CMemory *p_memory = CMemory::GetInstance();
	
	while(!m_MsgSendQueue.empty())
	{
		sTmpMempoint = m_MsgSendQueue.front();
		m_MsgSendQueue.pop_front(); 
		p_memory->FreeMemory(sTmpMempoint);
	}	
}


//在创建worker进程之前就要执行这个函数
bool CSocket::OpenListeningSocket()
{    
    int                isock;                //listen socket
    struct sockaddr_in serv_addr;            //服务器的地址结构体
    int                iport;                //listen port
    
    //获取配置信息
    CConfig *p_config = CConfig::GetInstance();

    //初始化相关
    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);  //监听本地所有的IP地址

    for(int i = 0; i < m_ListenPortCount; i++) //监听配置项中设定的端口
    {
        isock = socket(AF_INET,SOCK_STREAM,0);
        if(isock == -1)
        {
            ngx_log_stderr(errno,"【ngx_c_socket.cxx】打开第%d个监听端口的socket失败",i);
            close(isock);
            return false;
        }
        
        //SO_REUSEADDR：主要用于重启处于TIME_WAIT状态的服务器程序时，bind()失败的问题
        //TIME_WAIT状态持续时间为2MSL【最长数据包生存时间】，一般为1~4分钟 
        int reuseaddr = 1;  //1:打开对应的设置项
        if(setsockopt(isock,SOL_SOCKET, SO_REUSEADDR,(const void *) &reuseaddr, sizeof(reuseaddr)) == -1)
        {
            ngx_log_stderr(errno,"【ngx_c_socket.cxx】对第%d个端口的socket配置SO_REUSEADDR失败",i);
            close(isock);                                         
            return false;
        }

        //设置该socket为非阻塞
        int nb=1; //0：清除，1：设置
        if(ioctl(isock, FIONBIO, &nb) == -1) //FIONBIO：设置/清除非阻塞I/O标记：0：清除，1：设置
        {
            ngx_log_stderr(errno,"【ngx_c_socket.cxx】对第%d个端口的socket设置非阻塞失败",i);
            close(isock);
            return false;
        }

        //设置本服务器要监听的地址和端口，这样客户端才能连接到该地址和端口并发送数据
        char strinfo[100]; //临时字符串
        sprintf(strinfo,"ListenPort%d",i);
        iport = p_config->GetIntDefault(strinfo,10000);
        serv_addr.sin_port = htons((in_port_t)iport);

        //绑定服务器地址结构体
        if(bind(isock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
        {
            ngx_log_stderr(errno,"【ngx_c_socket.cxx】bind()失败,i=%d.",i);
            close(isock);
            return false;
        }
        
        //开始监听
        if(listen(isock,NGX_LISTEN_BACKLOG) == -1)
        {
            ngx_log_stderr(errno,"【ngx_c_socket.cxx】listen()失败,i=%d.",i);
            close(isock);
            return false;
        }

        lpngx_listening_t p_listensocketitem = new ngx_listening_t; 
        memset(p_listensocketitem,0,sizeof(ngx_listening_t)); 
        p_listensocketitem->port = iport;                          //记录下所监听的端口号
        p_listensocketitem->fd   = isock;                          //套接字句柄保存下来   
        ngx_log_error_core(NGX_LOG_INFO,0,"监听%d端口成功!",iport); //显示一些信息到日志中
        m_ListenSocketList.push_back(p_listensocketitem);          //加入到队列中
    } //end for(int i = 0; i < m_ListenPortCount; i++)

    if(m_ListenSocketList.size() <= 0)  //如果没有监听的端口则返回错误
        return false;
    return true;
}

//【该函数暂时用不到】关闭监听socket
void CSocket::CloseListeningSocket()
{
    for(int i = 0; i < m_ListenPortCount; i++) //要关闭这么多个监听端口
    {
        close(m_ListenSocketList[i]->fd);
        ngx_log_error_core(NGX_LOG_INFO,0,"关闭监听端口%d!",m_ListenSocketList[i]->port); //显示一些信息到日志中
    }
    return;
}

//将一个待发送消息传入发消息队列
void CSocket::msgSend(char *psendbuf) 
{
    CMemory *p_memory = CMemory::GetInstance();

    CLock lock(&m_sendMessageQueueMutex);

    //发送队列过大(比如客户端恶意不接受数据，就会导致这个队列越来越大)
    //为了服务器安全，放弃这次数据的发送(虽然有可能导致客户端出现问题，但总比服务器不稳定要好很多)
    if(m_iSendMsgQueueCount > 50000) 
    {
        m_iDiscardSendPkgCount++;
        p_memory->FreeMemory(psendbuf);
		return;
    }
    
    //发送队列在合理范围内，则看看该连接对应的客户端是否是恶意客户
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)psendbuf;
	lpngx_connection_t p_Conn = pMsgHeader->pConn;
    if(p_Conn->iSendCount > 400) //该客户端在发送队列中累积的任务数过多(可能是该用户收消息太慢或拒收消息)，将其判定为恶意用户，直接切断
    {
        ngx_log_stderr(0,"CSocket::msgSend()中发现某用户%d积压了大量待发送数据包，切断与他的连接！",p_Conn->fd);      
        m_iDiscardSendPkgCount++;
        p_memory->FreeMemory(psendbuf);
        zdClosesocketProc(p_Conn);
		return;
    }

    ++p_Conn->iSendCount; //发送队列中有的数据条目数+1；
    m_MsgSendQueue.push_back(psendbuf);     
    ++m_iSendMsgQueueCount;   //原子操作

    //将信号量的值+1,这样其他卡在sem_wait的就可以走下去
    if(sem_post(&m_semEventSendQueue)==-1)  //让ServerSendQueueThread()流程走下来干活
    {
        ngx_log_stderr(0,"CSocket::msgSend()中sem_post(&m_semEventSendQueue)失败.");      
    }
    return;
}

//主动关闭一个连接
//这个函数目前是被一个回收线程调用，所以不用考虑互斥的问题，如果将来用多个回收线程，那么就需要加锁
void CSocket::zdClosesocketProc(lpngx_connection_t p_Conn)
{
    if(m_ifkickTimeCount == 1)
    {
        DeleteFromTimerQueue(p_Conn); //把连接从时间队列中剔除
    }
    if(p_Conn->fd != -1)
    {   
        close(p_Conn->fd); //这个socket关闭，关闭后epoll就会被从红黑树中删除，所以这之后无法收到任何epoll事件
        p_Conn->fd = -1;
    }

    if(p_Conn->iThrowsendCount > 0)  
        --p_Conn->iThrowsendCount;

    inRecyConnectQueue(p_Conn);
    return;
}

//打印统计信息
void CSocket::printTDInfo()
{
    time_t currtime = time(NULL);
    if( (currtime - m_lastprintTime) > 10) //刚启动时打印一次，之后，每10s打印一次
    {
        int tmprmqc = g_threadpool.getRecvMsgQueueCount(); //收消息队列

        m_lastprintTime = currtime;
        int tmpoLUC = m_onlineUserCount;    //atomic做个中转，直接打印atomic类型报错；
        int tmpsmqc = m_iSendMsgQueueCount; //atomic做个中转，直接打印atomic类型报错；
        ngx_log_stderr(0,""); //换行
        ngx_log_stderr(0,"------------------------------------begin--------------------------------------");
        ngx_log_stderr(0,"当前连接数/允许最大连接数(%d/%d)。",tmpoLUC,m_worker_connections);        
        ngx_log_stderr(0,"连接池中空闲连接/总连接/要释放的连接(%d/%d/%d)。",m_freeconnectionList.size(),m_connectionList.size(),m_recyconnectionList.size());
        ngx_log_stderr(0,"当前时间队列大小(%d)。",m_timerQueuemap.size());        
        ngx_log_stderr(0,"当前收消息队列/发消息队列大小分别为(%d/%d)，丢弃的待发送数据包数量为%d。",tmprmqc,tmpsmqc,m_iDiscardSendPkgCount);        
        if(tmprmqc > 100000) 
        {
            ngx_log_stderr(0,"接收队列条目数量过大(%d)，要考虑限速或者增加处理线程数量了！！！！！！",tmprmqc);
        }
        ngx_log_stderr(0,"-------------------------------------end---------------------------------------");
    }
    
    return;
}

//epoll功能初始化，子进程中进行
int CSocket::ngx_epoll_init()
{
    //(1)创建epoll对象(很多内核版本不处理epoll_create的参数，只要该参数>0即可)
    m_epollhandle = epoll_create(m_worker_connections); 
    if (m_epollhandle == -1) 
    {
        ngx_log_stderr(errno,"CSocket::ngx_epoll_init()中epoll_create()失败.");
        exit(2); //这是致命问题了，直接退，资源由系统释放吧
    }

    //(2)创建连接池
    InitConnPool();

    //(3)遍历监听队列，为每个监听socket分配一个 连接池中的连接,便于记录该sokcet相关的信息
    std::vector<lpngx_listening_t>::iterator pos;	
	for(pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        lpngx_connection_t p_Conn = ngx_get_connection((*pos)->fd); //从连接池中获取一个空闲连接对象
        if (p_Conn == NULL) //致命问题，刚开始连接池不应该为空，记录异常
        {
            ngx_log_stderr(errno,"CSocket::ngx_epoll_init()中ngx_get_connection()失败.");
            exit(2);
        }
        p_Conn->listening = (*pos);   //连接对象和监听对象关联，方便通过连接对象找监听对象
        (*pos)->connection = p_Conn;  //监听对象和连接对象关联，方便通过监听对象找连接对象

        p_Conn->rhandler = &CSocket::ngx_event_accept; //对监听端口的读事件设置处理方法

        //(4)通过ngx_epoll_oper_event将监听socket的读事件添加到epoll中
        //EPOLLIN：可读，EPOLLRDHUP：TCP连接的远端关闭或者半关闭
        if(ngx_epoll_oper_event((*pos)->fd,EPOLL_CTL_ADD,EPOLLIN|EPOLLRDHUP,0,p_Conn) == -1) 
            exit(2);

    }
    return 1;
}

//对epoll事件的具体操作
//返回值：成功返回1，失败返回-1；
int CSocket::ngx_epoll_oper_event(
                        int                fd,               //socket句柄
                        uint32_t           eventtype,        //事件类型，一般是EPOLL_CTL_ADD，EPOLL_CTL_MOD，EPOLL_CTL_DEL ，说白了就是操作epoll红黑树的节点(增加，修改，删除)
                        uint32_t           flag,             //标志，具体含义取决于eventtype
                        int                bcaction,         //补充动作，用于补充flag标记的不足  :  0：增加   1：去掉 2：完全覆盖 ,eventtype是EPOLL_CTL_MOD时这个参数就有用
                        lpngx_connection_t pConn             //pConn：一个指针【其实是一个连接】，EPOLL_CTL_ADD时增加到红黑树中去，将来epoll_wait时能取出来用
                        )
{

    /**
     * ref:https://blog.csdn.net/chenycbbc0101/article/details/52471823
     * 
     *  struct epoll_event {
     *      __uint32_t events;  // Epoll events 
     *      epoll_data_t data;  // User data variable 
     *  };
     *_______________________________________________*/

    struct epoll_event ev;    
    memset(&ev, 0, sizeof(ev));

    if(eventtype == EPOLL_CTL_ADD) //往红黑树中增加节点；
    {
        ev.events = flag;      //既然是增加节点，则不管原来是啥标记
        pConn->events = flag;  //这个连接本身也记录这个标记
    }
    else if(eventtype == EPOLL_CTL_MOD)
    {
        ev.events = pConn->events;
        if(bcaction == 0) //增加
        {        
            ev.events |= flag; 
        }
        else if(bcaction == 1) //删除
        {
            ev.events &= ~flag;
        }
        else //覆盖
        {         
            ev.events = flag;          
        }
        pConn->events = ev.events; //记录该标记
    }
    //由于socket关闭，节点会自动从红黑树移除，故不需要显式的调用EPOLL_CTL_DEL

    //原来的理解中，绑定ptr这个事，只在EPOLL_CTL_ADD的时候做一次即可，但是发现内核源码对EPOLL_CTL_MOD
    //的实现会破坏掉.data.ptr，因此不管是EPOLL_CTL_ADD，还是EPOLL_CTL_MOD，都给进去
    ev.data.ptr = (void *)pConn;

    if(epoll_ctl(m_epollhandle,eventtype,fd,&ev) == -1)
    {
        ngx_log_stderr(errno,"CSocket::ngx_epoll_oper_event()中epoll_ctl(%d,%ud,%ud,%d)失败.",fd,eventtype,flag,bcaction);    
        return -1;
    }
    return 1;
}

//开始获取发生的事件消息
//参数unsigned int timer：epoll_wait()阻塞的时长，单位是毫秒；
//返回值，1：正常返回  ,0：有问题返回，一般不管是正常还是问题返回，都应该保持进程继续运行
//本函数被process_events_and_timers()调用，而process_events_and_timers()是在子进程的死循环中被反复调用
// 1为非正常返回，0为正常返回
int CSocket::ngx_epoll_process_events(int timer) 
{     
    /**
     * 关于epoll_wait():
     * 等待事件触发。被触发的事件会存储到m_events里，最多返回NGX_MAX_EVENTS个事件；
     * 阻塞timer这么长时间除非：a)阻塞时间到达 b)收到事件会立刻返回d)阻塞期间收到信号(比如kill -1 pid)
     * 如果timer为-1则一直阻塞，如果timer为0则立即返回，即便没有任何事件
     * 返回值：[1]有错误发生返回-1，错误在errno中；
     *        [2]如果等待一段时间，并且超时了，则返回0；
     *        [3]如果返回>0则表示成功捕获到这么多个事件
     */
    int events = epoll_wait(m_epollhandle,m_events,NGX_MAX_EVENTS,timer);    
    
    if(events == -1)
    {
        if(errno == EINTR)   //信号所致，正常返回。但需要提醒，因为一般也不会人为给worker进程发送消息
        {
            ngx_log_error_core(NGX_LOG_INFO,errno,"CSocket::ngx_epoll_process_events()中epoll_wait()失败!"); 
            return 1;
        }
        else
        {
            ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocket::ngx_epoll_process_events()中epoll_wait()失败!"); 
            return 0;
        }
    }

    if(events == 0) //超时
    {
        if(timer != -1) //要求epoll_wait阻塞一定的时间而不是一直阻塞，这属于阻塞到时间了，则正常返回
        {
            return 1;
        }
        //明明要求epoll_wait一直阻塞，却返回0，说明有问题      
        ngx_log_error_core(NGX_LOG_ALERT,0,"CSocket::ngx_epoll_process_events()中epoll_wait()没超时却没返回任何事件!"); 
        return 0;
    }

    //走到这里，就是说明有事件被触发了
    lpngx_connection_t p_Conn;
    uint32_t           revents;
    for(int i = 0; i < events; ++i)    //遍历本次epoll_wait返回的所有事件，注意events才是返回的实际事件数量
    {
        p_Conn = (lpngx_connection_t)(m_events[i].data.ptr);

        revents = m_events[i].events;

        if(revents & EPOLLIN)  //读事件来了
        {
            //一个客户端新连入，此处执行CSocket::ngx_event_accept() 
            //已连接发送数据来，此处执行CSocket::ngx_read_request_handler()             
            (this->* (p_Conn->rhandler) )(p_Conn);
        }
        
        if(revents & EPOLLOUT) //写事件来了
        {
            if(revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) //客户端关闭，如果服务器端收到一个写通知事件，则这个条件也可能成立
            {
                //EPOLLERR：对应的连接发生错误                    8     = 1000 
                //EPOLLHUP：对应的连接被挂起                      16    = 0001 0000
                //EPOLLRDHUP：表示TCP连接的远端关闭或者半关闭连接  8192   = 0010  0000  0000  0000
                --p_Conn->iThrowsendCount;                 
            }
            else
            {
                (this->* (p_Conn->whandler) )(p_Conn);   //如果有数据没有发送完毕，由系统驱动来发送，则这里执行的应该是 CSocket::ngx_write_request_handler()
            }            
        }
    }
    return 1;
}


/* * * * * * * * * * * *
 * 处理发送消息队列的线程
 * * * * * * * * * * * *
 * 由于本系统采用的是LT模式。因此，当socket可写时，会不停的触发socket可写的事件。解决方案：
 * 开始不把socket加入epoll，需要向socket写数据的时候，直接调用write或者send发送数据。
 * 如果返回EAGAIN，把socket加入epoll，并将socket的iThrowsendCount加1，此后发送队列中关于
 * 该连接的数据包都会直接跳过。在epoll的驱动下写数据，全部数据发送完毕后，再移出epoll，并将
 * iThrowsendCount还原回0.
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * ref:https://blog.csdn.net/weiwangchao_/article/details/43685671
 */
void* CSocket::ServerSendQueueThread(void* threadData)
{    
    ThreadItem *pThread = static_cast<ThreadItem*>(threadData);
    CSocket *pSocketObj = pThread->_pThis;
    int err;
    std::list <char *>::iterator posCur,posTmp,posEnd;
    
    char *pMsgBuf;	
    LPSTRUC_MSG_HEADER	pMsgHeader;
	LPCOMM_PKG_HEADER   pPkgHeader;
    lpngx_connection_t  p_Conn;
    unsigned short      itmp;
    ssize_t             sendsize;  

    CMemory *p_memory = CMemory::GetInstance();
    
    while(g_stopEvent == 0) //不退出
    {
        //在msgSend(), ngx_write_request_handler()和Shutdown_Worker()中调用sem_post
        //如果被某个信号中断，sem_wait也可能过早的返回，错误为EINTR；
        if(sem_wait(&pSocketObj->m_semEventSendQueue) == -1)
        {
            if(errno != EINTR) 
                ngx_log_stderr(errno,"ServerSendQueueThread()中sem_wait(&pSocketObj->m_semEventSendQueue)失败.");            
        }
        
        //接下来线程开始发送数据
        if(g_stopEvent != 0)  //再判断一次是否需要退出
            break;

        if(pSocketObj->m_iSendMsgQueueCount > 0) //原子的 
        {
            err = pthread_mutex_lock(&pSocketObj->m_sendMessageQueueMutex);    
            if(err != 0) ngx_log_stderr(err,"ServerSendQueueThread()中pthread_mutex_lock()失败，返回的错误码为%d!",err);

            posCur    = pSocketObj->m_MsgSendQueue.begin();
			posEnd = pSocketObj->m_MsgSendQueue.end();

            while(posCur != posEnd)
            {
                pMsgBuf = (*posCur);
                pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;
                pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf+sizeof(STRUC_MSG_HEADER));
                p_Conn = pMsgHeader->pConn;

                //包过期
                if(p_Conn->iCurrsequence != pMsgHeader->iCurrsequence) 
                {
                    posTmp=posCur;
                    posCur++;
                    pSocketObj->m_MsgSendQueue.erase(posTmp);
                    --pSocketObj->m_iSendMsgQueueCount;	
                    p_memory->FreeMemory(pMsgBuf);	
                    continue;
                }

                //该连接的数据包发送，已交付至epoll负责，故跳过当前数据包
                if(p_Conn->iThrowsendCount > 0) 
                {
                    posCur++;
                    continue;
                }

                --p_Conn->iSendCount;   //发送队列中有的数据条目数-1；
            
                p_Conn->psendMemPointer = pMsgBuf;      //发送后释放用的，因为这段内存是new出来的
                posTmp=posCur;
				posCur++;
                pSocketObj->m_MsgSendQueue.erase(posTmp);
                --pSocketObj->m_iSendMsgQueueCount;
                p_Conn->psendbuf = (char *)pPkgHeader;
                itmp = ntohs(pPkgHeader->pkgLen);        //包头+包体 长度 ，打包时用了htons【本机序转网络序】，所以这里为了得到该数值，用了个ntohs【网络序转本机序】；
                p_Conn->isendlen = itmp;
                                
                //发送数据
                sendsize = pSocketObj->sendproc(p_Conn,p_Conn->psendbuf,p_Conn->isendlen);
                if(sendsize > 0)
                {                    
                    if(sendsize == p_Conn->isendlen) //说明一次性就把整个数据包发送完毕
                    {
                        p_memory->FreeMemory(p_Conn->psendMemPointer);
                        p_Conn->psendMemPointer = NULL;  
                    }
                    else  //数据发送了一部分时，发送缓冲区被填满，记录发送的位置
                    {
                        p_Conn->psendbuf = p_Conn->psendbuf + sendsize;
				        p_Conn->isendlen = p_Conn->isendlen - sendsize;	
                        ++p_Conn->iThrowsendCount; //标记一下，告知工作线程该连接的发送任务可以跳过
                        pSocketObj->ngx_epoll_oper_event(p_Conn->fd,EPOLL_CTL_MOD,EPOLLOUT,0,p_Conn); //交付epoll托管
                    } 
                    continue;  //继续处理其他消息                    
                } 
                else if(sendsize == 0)
                {
                    //由于本系统设置的发送数据包始终大于0字节，所以发送0字节的数据是不太可能的。若真有该现象发生，有必要提示一下
                    ngx_log_stderr(errno,"【不合逻辑】ServerSendQueueThread()中sendproc()居然返回0?");
                    p_memory->FreeMemory(p_Conn->psendMemPointer);
                    p_Conn->psendMemPointer = NULL;
                    continue;
                }
                else if(sendsize == -1)  //一个字节都没发送，发送缓冲区就满了
                {
                    ++p_Conn->iThrowsendCount;
                    pSocketObj->ngx_epoll_oper_event(p_Conn->fd,EPOLL_CTL_MOD,EPOLLOUT,0,p_Conn);
                    continue;
                }
                else //能走到这里的，应该就是返回值-2了，一般就认为对端断开了，等待recv()来做断开socket以及回收资源
                {
                    p_memory->FreeMemory(p_Conn->psendMemPointer);
                    p_Conn->psendMemPointer = NULL; 
                    continue;
                }

            } //end while(posCur != posEnd)
            err = pthread_mutex_unlock(&pSocketObj->m_sendMessageQueueMutex); 
            if(err != 0)  ngx_log_stderr(err,"ServerSendQueueThread()pthread_mutex_unlock()失败，返回的错误码为%d!",err);
            
        } //if(pSocketObj->m_iSendMsgQueueCount > 0)
    } //end while(g_stopEvent == 0)
    return (void*)0;
}
