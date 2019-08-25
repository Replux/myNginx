# 我为啥做这个项目
在学习了网络编程和服务器开发的知识后，我就去学习了Nginx的源码。这个项目早期主要参考nginx编写的，随后结合自己的需求，以及对nginx一些代码的改进，最后产生了这个项目。

# 我参考了nginx的哪些东西？
1. 一个master，多个worker的进程结构;
2. 参考了nginx写的epoll相关代码，但是我是用LT模式来对事件进行处理的，而Nginx是采用ET模式;

# 使用介绍
1. 在根目录下直接"make"编译项目后会生成名为"nginx"的可执行文件，运行该文件即可。
2. 可在"nginx.conf"配置文件中对该程序进行调整。
3. 压力测试的Demo放在"TestServerDemo/MFCApplication4"文件夹下。
4. 系统的核心类有两个,分别是CSocket和CLogicSocket。CSocket是CLogicSocket的父类，前者提供了本系统的核心功能，后者是对前者在业务逻辑功能上的扩展。因此，如果需要扩展本系统可以继承CSocket类，或者是在CLogicSocket类下编写相关业务逻辑。

# 技术要点
1. 使用Epoll水平触发的IO多路复用技术，并采用Reactor模式对事件进行处理；
2. 通过线程池来处理不同的业务逻辑，避免线程频繁创建销毁的开销；
3. 主线程负责监听/接收事件，并将事件插入到请求队列中，由工作线程从请求队列中获取事件进行处理；
4. 根据收包情况对每条连接进行评估，以预防SYN Flood攻击和畸形数据报等网络攻击；
5. 实现了基于堆的定时器，用于对客户端进行心跳检测以及限时连接；
6. 支持配置文件的读取与系统日志打印；
7. 复用网络连接的上下文信息，减少动态申请/销毁内存的次数；

# 系统结构
![model](https://github.com/Replux/myNginx/blob/master/reactor.png)

# 核心函数调用关系
+ main() //程序入口
    + ngx_master_process_cycle()  //创建子进程等一系列动作
        + ngx_setproctitle()       //设置进程标题    
        + ngx_start_worker_processes() //创建worker子进程   
            + for (i = 0; i < threadnums; i++)  //master进程在走这个循环，来创建若干个子进程
                + ngx_spawn_process(i,"worker process");
                    + pid = fork(); //创建子进程
	                //只有子进程才会执行ngx_worker_process_cycle()
	                + ngx_worker_process_cycle(inum,pprocname);
	                    + ngx_worker_process_init();
	                        + sigemptyset(&set);  
                            + sigprocmask(SIG_SETMASK, &set, NULL); //允许接收所有信号
                            + g_threadpool.Create(tmpthreadnums);  //创建线程池中线程
                            + _socket.Initialize_subproc();  //初始化子进程需要具备的一些多线程能力相关的信息
                            + g_socket.ngx_epoll_init();  //初始化epoll相关内容，同时往监听socket上增加监听事件
                                + m_epollhandle = epoll_create(m_worker_connections); 
                                + ngx_epoll_add_event((*pos)->fd....);
                                    + epoll_ctl(m_epollhandle,eventtype,fd,&ev);
                        + ngx_setproctitle(pprocname);         //重新为子进程设置标题为worker process
                        + for(;;)
                            + ngx_process_events_and_timers(); //处理网络事件和定时器事件 
                                + g_socket.ngx_epoll_process_events(-1); //-1表示无限时等待
                                    + epoll_wait();
                        + g_threadpool.StopAll();      //考虑在这里停止线程池；
		    			+ g_socket.Shutdown_subproc(); //socket需要释放的东西考虑释放；	
        + sigemptyset(&set); 
        + for ( ;; ) {}  //master会一直在这里循环
