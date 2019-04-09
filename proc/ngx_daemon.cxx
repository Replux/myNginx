#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>    
#include <sys/stat.h>
#include <fcntl.h>


#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"

//描述：将调用该函数的进程转变为守护进程
//父进程：返回1
//子进程：成功返回0，失败返回-1
int ngx_daemon()
{
    //(1)创建守护进程的第一步，fork()一个子进程出来
    switch (fork())
    {
    case -1: //创建子进程失败
        ngx_log_error_core(NGX_LOG_EMERG,errno, "ngx_daemon()中fork()失败!");
        return -1;
    case 0: //子进程，走到这里直接break;
        break;
    default:
        return 1;  //父进程回到主流程中去释放一些资源
    }

    //只有fork()出来的子进程运行至此
    ngx_parent = ngx_pid; 
    ngx_pid = getpid();
    
    //(2)脱离终端
    if (setsid() == -1)  
    {
        ngx_log_error_core(NGX_LOG_EMERG, errno,"ngx_daemon()中setsid()失败!");
        return -1;
    }

    //(3)关闭文件权限
    umask(0); 

    //(4)以读写方式打开黑洞设备
    int fd = open("/dev/null", O_RDWR);
    if (fd == -1) 
    {
        ngx_log_error_core(NGX_LOG_EMERG,errno,"ngx_daemon()中打开黑洞设备失败!");        
        return -1;
    }
    if (dup2(fd, STDIN_FILENO) == -1) //让标准输入指向黑洞
    {
        ngx_log_error_core(NGX_LOG_EMERG,errno,"ngx_daemon()中dup2(STDIN)失败!");        
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) == -1) //让标准输出指向黑洞
    {
        ngx_log_error_core(NGX_LOG_EMERG,errno,"ngx_daemon()中dup2(STDOUT)失败!");
        return -1;
    }
    if (fd > STDERR_FILENO)  //由于标准输入/输出/错误fd分别是0,1,2且没被关闭，故正常情况下该条件成立
    {
        if (close(fd) == -1)  //释放资源描述符
        {
            ngx_log_error_core(NGX_LOG_EMERG,errno, "ngx_daemon()中close(fd)失败!");
            return -1;
        }
    }
    else
    {
        ngx_log_error_core(NGX_LOG_EMERG,errno, "标准输入/输出，提前被关闭，请检查异常");
    }
    
    return 0; //子进程返回0
}

