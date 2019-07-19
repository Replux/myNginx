#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

#include "ngx_global.h"
#include "ngx_macro.h"
#include "ngx_func.h" 

//一个信号有关的结构 ngx_signal_t
typedef struct 
{
    int           signo;
    const  char   *signame;    //信号对应的中文名字 ，比如SIGHUP 
    void  (*handler)(int signo, siginfo_t *siginfo, void *ucontext); //函数指针,   siginfo_t:系统定义的结构
} ngx_signal_t;

//声明一个信号处理函数
static void ngx_signal_handler(int signo, siginfo_t *siginfo, void *ucontext); //static表示该函数只在当前文件内可见
static void ngx_process_get_status(void);                                      //获取子进程的结束状态，防止单独kill子进程时子进程变成僵尸进程


ngx_signal_t  signals[] = {
    // signo      signame             handler
    { SIGHUP,    "SIGHUP",           ngx_signal_handler },        //终端断开信号，对于守护进程常用于reload重载配置文件通知--标识1
    { SIGINT,    "SIGINT",           ngx_signal_handler },        //标识2   
	{ SIGTERM,   "SIGTERM",          ngx_signal_handler },        //标识15
    { SIGCHLD,   "SIGCHLD",          ngx_signal_handler },        //子进程退出时，父进程会收到这个信号--标识17
    { SIGQUIT,   "SIGQUIT",          ngx_signal_handler },        //标识3
    { SIGIO,     "SIGIO",            ngx_signal_handler },        //指示一个异步I/O事件【通用异步I/O信号】
    { SIGSYS,    "SIGSYS, SIG_IGN",  NULL               },        //忽略该信号
    
    //...日后根据需要再继续增加
    { 0,         NULL,               NULL               }         //信号对应的数字至少是1，所以可以用0作为一个特殊标记
};

//初始化信号的函数，用于注册信号处理程序
//返回值：0成功, -1失败
int ngx_init_signals()
{
    /**
     * struct sigaction{
     *     void (*sa_handler)(int);
     *     sigset_t sa_mask;
     *     int sa_flag;
     *     void (*sa_sigaction)(int,siginfo_t*,void*);
     * };
     */
    ngx_signal_t      *sig;  //指向自定义结构数组的指针 
    struct sigaction   sa;

    for (sig = signals; sig->signo != 0; sig++)  //将signo ==0作为一个标记，因为信号的编号都不为0；
    {
        memset(&sa,0,sizeof(struct sigaction));

        if (sig->handler)
        {
            sa.sa_sigaction = sig->handler;  //sa_sigaction是函数指针，指定信号处理函数
            sa.sa_flags = SA_SIGINFO; //该标志位可以看成信号是否传递参数的开关，如果设置该位，则传递参数；否则，不传递参数
        }
        else
        {
            sa.sa_handler = SIG_IGN; //标记SIG_IGN给到sa_handler成员，表示忽略信号处理程序，否则os的默认信号处理程序会把这个进程杀掉；                                       
        } 

        sigemptyset(&sa.sa_mask);        
        
        if (sigaction(sig->signo,  //要操作的信号
                        &sa,       //相关配置(信号处理函数以及要屏蔽的信号)
                        NULL       //返回以往的对信号的处理方式，不需要则设为NULL
                        ) == -1)
        {   
            ngx_log_error_core(NGX_LOG_EMERG,errno,"sigaction(%s) failed",sig->signame); //显示到日志文件中去的 
            return -1; //有失败就直接返回
        }	
    }
    return 0;   
}

//信号处理函数
//siginfo：这个系统定义的结构中包含了信号产生原因的有关信息
static void ngx_signal_handler(int signo, siginfo_t *siginfo, void *ucontext)
{    
    ngx_signal_t    *sig;    //自定义结构
    
    for (sig = signals; sig->signo != 0; sig++) //遍历信号表，找到对应信号即可处理
    {         
        if (sig->signo == signo)
            break; //do nothing
    } 

    if(ngx_process == PROCESS_MASTER)      //master进程，管理进程，处理的信号一般会比较多 
    {
        switch (signo)
        {
        case SIGCHLD:  //一般子进程退出会收到该信号
            ngx_reap = 1;  //标记子进程状态变化，日后master主进程的for(;;)循环中可能会用到这个变量【比如重新产生一个子进程】
            break;
        //.....其他信号处理待补充
        default:
            break;
        }
    }
    else if(ngx_process == PROCESS_WORKER) //worker进程，具体干活的进程，处理的信号相对比较少
    {
        //worker的响应逻辑，待补充
    }
    else
    {
        //非master非worker的响应逻辑，待补充
    }

    //这里记录一些日志信息
    if(siginfo && siginfo->si_pid)  //si_pid = sending process ID【发送该信号的进程id】
        ngx_log_error_core(NGX_LOG_NOTICE,0,"signal %d (%s) received from %P%s", signo, sig->signame, siginfo->si_pid); 
    else
        ngx_log_error_core(NGX_LOG_NOTICE,0,"signal %d (%s) received %s",signo, sig->signame);//没有发送该信号的进程id，所以不显示发送该信号的进程id

    //...待扩展

    if (signo == SIGCHLD) 
        ngx_process_get_status(); //获取子进程的结束状态

    return;
}

//获取子进程的结束状态，防止单独kill子进程时子进程变成僵尸进程
static void ngx_process_get_status(void)
{
    pid_t            pid;
    int              status;
    int              err;

    for ( ;; ) 
    {
        //第一次waitpid返回一个> 0值，表示成功，后边显示 2019/01/14 21:43:38 [alert] 3375: pid = 3377 exited on signal 9【SIGKILL】
        //第二次再循环回来，再次调用waitpid会返回一个0，表示子进程还没结束，然后这里有return来退出；
        pid = waitpid(-1, &status, WNOHANG); //第一个参数为-1，表示等待任何子进程，
                                              //第二个参数：保存子进程的状态信息
                                               //第三个参数：提供额外选项，WNOHANG表示不要阻塞，让这个waitpid()立即返回        

        if(pid == 0) //子进程没结束，会立即返回这个数字，但这里应该不是这个数字【因为一般是子进程退出时会执行到这个函数】
            return;
            
        if(pid == -1) //这里处理代码抄自官方nginx，主要目的是打印一些日志
        {
            err = errno;

            if(err == EINTR)           //调用被某个信号中断
                continue;

            if(err == ECHILD)  //没有子进程
                return;

            if (err == ECHILD)         //没有子进程
            {
                ngx_log_error_core(NGX_LOG_INFO,err,"waitpid() failed!");
                return;
            }

            ngx_log_error_core(NGX_LOG_ALERT,err,"waitpid() failed!");
            return;
        }
        
        //走到这里，表示成功【返回进程id】 ，最后打印一些日志来记录子进程的退出

        if(WTERMSIG(status))  //获取使子进程终止的信号编号
            ngx_log_error_core(NGX_LOG_ALERT,0,"pid = %P exited on signal %d!",pid,WTERMSIG(status)); //获取使子进程终止的信号编号
        else
            ngx_log_error_core(NGX_LOG_NOTICE,0,"pid = %P exited with code %d!",pid,WEXITSTATUS(status)); //WEXITSTATUS()用于获取子进程传递给exit或者_exit参数的低八位
    } //end for ( ;; ) 
    return;
}
