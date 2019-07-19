//和开启子进程相关
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>   //信号相关头文件 
#include <errno.h>    //errno
#include <unistd.h>

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"

//函数声明
static void start_worker_processes(int threadnums);
static int  spawn_process(int threadnums,const char *pprocname);
static void worker_process_cycle(int inum,const char *pprocname);
static void ngx_worker_process_init(int inum);

//变量声明
static u_char  masterTitle[] = "master process";

//描述：创建worker子进程
void ngx_master_process_cycle()
{    
    sigset_t set;        //信号集
    sigemptyset(&set);   //清空信号集

    //在执行本函数期间不希望被下列信号干扰
    sigaddset(&set, SIGCHLD);     //子进程状态改变
    sigaddset(&set, SIGALRM);     //定时器超时
    sigaddset(&set, SIGIO);       //异步I/O
    sigaddset(&set, SIGINT);      //终端中断符
    sigaddset(&set, SIGHUP);      //连接断开
    sigaddset(&set, SIGUSR1);     //用户定义信号
    sigaddset(&set, SIGUSR2);     //用户定义信号
    sigaddset(&set, SIGWINCH);    //终端窗口大小改变
    sigaddset(&set, SIGTERM);     //终止
    sigaddset(&set, SIGQUIT);     //终端退出符
    
    if (sigprocmask(SIG_BLOCK, &set, NULL) == -1) 
    {        
        ngx_log_error_core(NGX_LOG_ALERT,errno,"【ngx_master_process_cycle.cxx】sigprocmask()失败");
        //即便sigprocmask失败，也要继续走下去
    }
    

    //首先设置主进程标题
    size_t size;
    int    i;
    size = sizeof(masterTitle);
    size += g_argvneedmem;          //argv参数长度加进来    
    if(size < 1000)
    {
        char title[1000] = {0};
        strcpy(title,(const char *)masterTitle); //"master process"
        strcat(title," ");  //跟一个空格分开一些，清晰    //"master process "

        for (i = 0; i < g_argc; i++)         //"master process ./nginx"
            strcat(title,g_argv[i]);

        ngx_setproctitle(title); //设置标题
        ngx_log_error_core(NGX_LOG_NOTICE,0,"%s %p 【master进程】启动并开始运行......!",title,ngx_pid); //设置标题时顺便记录下来进程名，进程id等信息到日志
    }    
    
    CConfig *p_config = CConfig::GetInstance();
    int workprocess = p_config->GetIntDefault("WorkerProcesses",1); //从配置文件中读取要创建的worker进程数量
    start_worker_processes(workprocess);  //创建worker子进程

    //创建子进程后，父进程的执行流程会返回到这里，子进程不会走到这  
    sigemptyset(&set); //取消信号屏蔽
    
    for ( ;; ) 
    {
        // sigsuspend(const sigset_t *mask))用于在接收到某个信号之前, 临时用mask替换进程的信号掩码, 并暂停进程执行，直到收到信号为止。
        // sigsuspend 返回后将恢复调用之前的信号掩码。信号处理函数完成后，进程将继续执行。该系统调用始终返回-1，并将errno设置为EINTR。

        //sigsuspend是原子操作，包含4步：
        //a)根据给定的参数设置新的mask【空集】并阻塞当前进程
        //b)一旦收到信号A，便恢复原先的信号屏蔽【即之前设置的屏蔽10个信号，从而保证下面的流程不会再次被其他信号截断】
        //c)调用信号A对应的信号处理函数
        //d)信号处理函数返回后，sigsuspend返回，使程序流程继续往下走
        sigsuspend(&set);  //master挂起（不占用CPU时间），依赖于信号驱动
        
        sleep(1); 
    }
    return;
}

//描述：根据给定的参数创建指定数量的子进程，因为以后可能要扩展功能，增加参数，所以单独写成一个函数
//threadnums:要创建的子进程数量
static void start_worker_processes(int threadnums)
{
    for (int i = 0; i < threadnums; i++)  //master进程在走这个循环，来创建若干个子进程
        spawn_process(i,"worker process");

    return;
}

//描述：产生一个子进程
//inum：进程编号【0开始】
//pprocname：子进程名字"worker process"
static int spawn_process(int inum,const char *pprocname)
{
    pid_t  pid = fork();
    switch (pid)
    {  
    case -1: //产生子进程失败
        ngx_log_error_core(NGX_LOG_ALERT,errno,"spawn_process()fork()产生子进程num=%d,procname=\"%s\"失败!",inum,pprocname);
        return -1;

    case 0:  //子进程分支
        ngx_parent = ngx_pid;                        //因为是子进程了，所有原来的pid变成了父pid
        ngx_pid = getpid();                          //重新获取pid,即本子进程的pid
        worker_process_cycle(inum,pprocname);    //所有worker子进程在该函数中循环工作
        break;

    default: //父进程分支         
        break;
    }

    return pid;
}

//描述：worker子进程的功能函数，无限循环【处理网络事件和定时器事件以对外提供服务】
//inum：进程编号【0开始】
static void worker_process_cycle(int inum,const char *pprocname) 
{
    //设置一下变量
    ngx_process = PROCESS_WORKER;  //设置进程的类型，是worker进程

    //重新为子进程设置进程名，不要与父进程重复------
    ngx_worker_process_init(inum);
    ngx_setproctitle(pprocname); //设置标题   
    ngx_log_error_core(NGX_LOG_NOTICE,0,"%s %p 【worker进程】启动并开始运行......!",pprocname,ngx_pid); //设置标题时顺便记录下来进程名，进程id等信息到日志

    for(;;)
    {
        process_events_and_timers(); //处理网络事件和定时器事件
    } 

    //如果从这个循环跳出来
    g_threadpool.StopAll();      //考虑在这里停止线程池；
    g_socket.Shutdown_Worker(); //socket需要释放的东西考虑释放；
    return;
}

//描述：子进程创建时调用本函数进行一些初始化工作
static void ngx_worker_process_init(int inum)
{
    sigset_t  set;      //信号集

    sigemptyset(&set);  //清空信号集
    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1)  //取消信号屏蔽
    {
        ngx_log_error_core(NGX_LOG_ALERT,errno,"ngx_worker_process_init()中sigprocmask()失败!");
    }

    //线程池代码，率先创建，至少要比和socket相关的内容优先
    CConfig *p_config = CConfig::GetInstance();
    int threadNum = p_config->GetIntDefault("ProcMsgRecvWorkThreadCount",3);
    if(g_threadpool.Create(threadNum) == false)  //创建线程池中线程
    {
        exit(-2);
    }
    
    sleep(1); //休息1秒；

    if(g_socket.Initialize_Worker() == false) //初始化子进程需要具备的一些多线程能力相关的信息
    {
        exit(-2);
    }
    
    g_socket.ngx_epoll_init(); //初始化epoll相关内容，同时 往监听socket上增加监听事件，从而开始让监听端口履行其职责

    return;
}
