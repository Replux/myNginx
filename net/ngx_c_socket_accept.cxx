
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
    struct sockaddr    mysockaddr;        //远端服务器的socket地址
    socklen_t          socklen;
    int                err;
    int                level;
    int                s;
    static int         use_accept4 = 1;   //有的内核版本可能不支持accept4
    lpngx_connection_t newc;              //代表连接池中的一个连接
    
    socklen = sizeof(mysockaddr);
    do
    {     
        if(use_accept4)
            s = accept4(oldc->fd, &mysockaddr, &socklen, SOCK_NONBLOCK);
        else
            s = accept(oldc->fd, &mysockaddr, &socklen);

        if(s == -1)
        {
            err = errno;

            if(err == EAGAIN)
                return ;

            level = NGX_LOG_ALERT;
            if (err == ECONNABORTED)  //ECONNRESET错误则发生在对方意外关闭套接字后(用户插拔网线就可能有这个错误出现)
            {
                level = NGX_LOG_ERR;
            } 
            else if (err == EMFILE || err == ENFILE) 
            {
                //EMFILE:进程的fd已用尽【已达到系统所允许单一进程所能打开的文件/套接字总数】
                //ENFILE这个errno的存在，表明存在resource limits。
                level = NGX_LOG_CRIT;
            }

            if(use_accept4 && err == ENOSYS) //ENOSYS表示系统不支持accept4
            {
                use_accept4 = 0;
                continue;
            }

            if (err == ECONNABORTED)  //对方关闭套接字，返回RST
            {
                //do nothing
            }
            
            if (err == EMFILE || err == ENFILE) 
            {
                //do nothing
            }            
            return;
        }  //end if(s == -1)
      
        if(m_onlineUserCount >= m_worker_connections)  //用户连接数过多，则关闭该用户socket
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
                ngx_log_error_core(NGX_LOG_ALERT,errno,"CSocket::ngx_event_accept()中close(%d)失败!",s);                
           
            return;
        }

        //...将来此处可以判断是否连接超过最大允许连接数

        //成功的拿到了连接池中的一个连接
        memcpy(&newc->s_sockaddr,&mysockaddr,socklen);

        if(!use_accept4) //如果不是用accept4()取得的socket，那么就要设置为非阻塞
        {
            int nb=1; //0：清除，1：设置
            if(ioctl(s, FIONBIO, &nb) == -1)
            {
                ngx_log_stderr(errno,"【创建失败】ngx_c_socket_accept中对socket设置非阻塞失败");
                ngx_close_connection(newc); //关闭socket,立即回收该连接，由于没有发送数据所以无需延迟
                return; //直接返回
            }
        }

        newc->listening = oldc->listening;  //连接对象和监听对象关联
        
        newc->rhandler = &CSocket::ngx_read_request_handler;
        newc->whandler = &CSocket::ngx_write_request_handler;

        //客户端应该主动发送第一次的数据，这里将读事件加入epoll监控，这样当客户端发送数据来时，会触发ngx_wait_request_handler()被ngx_epoll_process_events()调用        
        if(ngx_epoll_oper_event(s,EPOLL_CTL_ADD,EPOLLIN|EPOLLRDHUP,0,newc) == -1)         
        {
            ngx_close_connection(newc); //关闭socket,立即回收该连接，无需延迟
            return;
        }


        if(m_ifkickTimeCount == 1)
            AddToTimerQueue(newc);

        ++m_onlineUserCount;  //连入用户数量+1        
        break;  //一般就是循环一次就跳出去
    } while (1);   

    return;
}

