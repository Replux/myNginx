﻿
//和网络中接收连接【accept】有关的函数放这里
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>    //uintptr_t
#include <stdarg.h>
#include <unistd.h> 
#include <sys/time.h>  //gettimeofday
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

//建立新连接专用函数，当新连接进入时，本函数会被ngx_epoll_process_events()所调用
void CSocket::ngx_event_accept(lpngx_connection_t oldc)
{
    //这也可以避免本函数被卡太久，注意，本函数应该尽快返回，以免阻塞程序运行；
    struct sockaddr    mysockaddr;        //远端服务器的socket地址
    socklen_t          socklen;
    int                err;
    int                level;
    int                s;
    static int         use_accept4 = 1;
    lpngx_connection_t newc;              //代表连接池中的一个连接【注意这是指针】
    
    socklen = sizeof(mysockaddr);
    do   //用do，跳到while后边去方便
    {     
        if(use_accept4)
        {
            //以为listen套接字是非阻塞的，所以即便已完成连接队列为空，accept4()也不会卡在这里；
            s = accept4(oldc->fd, &mysockaddr, &socklen, SOCK_NONBLOCK); //从内核获取一个用户端连接，最后一个参数SOCK_NONBLOCK表示返回一个非阻塞的socket，节省一次ioctl【设置为非阻塞】调用
        }
        else
        {
            //以为listen套接字是非阻塞的，所以即便已完成连接队列为空，accept()也不会卡在这里；
            s = accept(oldc->fd, &mysockaddr, &socklen);
        }

        if(s == -1)
        {
            err = errno;

            //对accept、send和recv而言，事件未发生时errno通常被设置成EAGAIN（意为“再来一次”）或者EWOULDBLOCK（意为“期待阻塞”）
            if(err == EAGAIN) //accept()没准备好，这个EAGAIN错误EWOULDBLOCK是一样的
            {
                return ;
            } 
            level = NGX_LOG_ALERT;
            if (err == ECONNABORTED)  //ECONNRESET错误则发生在对方意外关闭套接字后【您的主机中的软件放弃了一个已建立的连接--由于超时或者其它失败而中止接连(用户插拔网线就可能有这个错误出现)】
            {
                level = NGX_LOG_ERR;
            } 
            else if (err == EMFILE || err == ENFILE) //EMFILE:进程的fd已用尽【已达到系统所允许单一进程所能打开的文件/套接字总数】。可参考：https://blog.csdn.net/sdn_prc/article/details/28661661   以及 https://bbs.csdn.net/topics/390592927
                                                        //ulimit -n ,看看文件描述符限制,如果是1024的话，需要改大;  打开的文件句柄数过多 ,把系统的fd软限制和硬限制都抬高.
                                                    //ENFILE这个errno的存在，表明一定存在system-wide的resource limits，而不仅仅有process-specific的resource limits。按照常识，process-specific的resource limits，一定受限于system-wide的resource limits。
            {
                level = NGX_LOG_CRIT;
            }

            if(use_accept4 && err == ENOSYS) //accept4()函数没实现，坑爹？
            {
                use_accept4 = 0;  //标记不使用accept4()函数，改用accept()函数
                continue;         //回去重新用accept()函数搞
            }

            if (err == ECONNABORTED)  //对方关闭套接字
            {
                //这个错误因为可以忽略，所以不用干啥
                //do nothing
            }
            
            if (err == EMFILE || err == ENFILE) 
            {
                //do nothing
            }            
            return;
        }  //end if(s == -1)

        //走到这里的，表示accept4()/accept()成功了        
        if(m_onlineUserCount >= m_worker_connections)  //用户连接数过多，要关闭该用户socket，因为现在也没分配连接，所以直接关闭即可
        {
            close(s);
            return ;
        }
        if(m_connectionList.size() > (m_worker_connections * 5))
        {
            if(m_freeconnectionList.size() < m_worker_connections)
            {
                close(s);
                return ;   
            }
        }

        newc = ngx_get_connection(s); //这是针对新连入用户的连接，和监听套接字 所对应的连接是两个不同的东西，不要搞混
        if(newc == NULL)
        {
            //连接池中连接不够用，那么就得把这个socket直接关闭并返回了，因为在ngx_get_connection()中已经写日志了，所以这里不需要写日志了
            if(close(s) == -1)
            {
                ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocket::ngx_event_accept()中close(%d)失败!",s);                
            }
            return;
        }
        //...将来这里会判断是否连接超过最大允许连接数，现在，这里可以不处理

        //成功的拿到了连接池中的一个连接
        memcpy(&newc->s_sockaddr,&mysockaddr,socklen);

        if(!use_accept4)
        {
            //如果不是用accept4()取得的socket，那么就要设置为非阻塞【因为用accept4()的已经被accept4()设置为非阻塞了】
            int nb=1; //0：清除，1：设置
            if(ioctl(s, FIONBIO, &nb) == -1) //设置非阻塞居然失败
            {
                ngx_log_stderr(errno,"【创建失败】ngx_c_socket_accept中对socket设置非阻塞失败");
                ngx_close_connection(newc); //关闭socket,这种可以立即回收这个连接，无需延迟，因为其上还没有数据收发，谈不到业务逻辑因此无需延迟；
                return; //直接返回
            }
        }

        newc->listening = oldc->listening;                    //连接对象 和监听对象关联，方便通过连接对象找监听对象【关联到监听端口】
        
        newc->rhandler = &CSocket::ngx_read_request_handler;  //设置数据来时的读处理函数，其实官方nginx中是ngx_http_wait_request_handler()
        newc->whandler = &CSocket::ngx_write_request_handler; //设置数据发送时的写处理函数。

        //客户端应该主动发送第一次的数据，这里将读事件加入epoll监控，这样当客户端发送数据来时，会触发ngx_wait_request_handler()被ngx_epoll_process_events()调用        
        if(ngx_epoll_oper_event(
                                s,                  //socket句柄
                                EPOLL_CTL_ADD,      //事件类型，这里是增加
                                EPOLLIN|EPOLLRDHUP, //标志，这里代表要增加的标志,EPOLLIN：可读，EPOLLRDHUP：TCP连接的远端关闭或者半关闭 ，如果边缘触发模式可以增加 EPOLLET
                                0,                  //对于事件类型为增加的，不需要这个参数
                                newc                //连接池中的连接
                                ) == -1)         
        {
            ngx_close_connection(newc);//关闭socket,这种可以立即回收这个连接，无需延迟，因为其上还没有数据收发，谈不到业务逻辑因此无需延迟；
            return; //直接返回
        }


        if(m_ifkickTimeCount == 1)
        {
            AddToTimerQueue(newc);
        }
        ++m_onlineUserCount;  //连入用户数量+1        
        break;  //一般就是循环一次就跳出去
    } while (1);   

    return;
}
