#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "lst_timer.h"

#define MAX_FD 65535  // 最大文件描述符个数
#define MAX_EVENTS_NUMBER 10000 // 监听最大事件数量
static int pipefd[2]; 

// 添加信号捕捉
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

// 向管道写数据的信号捕捉回调函数
void sig_to_pipe(int sig){
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

// 添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);
// 从epoll中删除
extern void removefd(int epollfd,int fd);
// 修改文件描述符
extern void modfd(int epollfd, int fd, int ev);

extern void setnonblocking(int fd);

int main(int argc, char* argv[]) {

    if(argc <= 1) {
        printf("按照如下格式运行：%s port_number\n",basename(argv[0]));
        exit(-1);
    }

    //端口号
    int port = atoi(argv[1]);

    // 对SIGPIPE信号进行捕捉
    addsig(SIGPIPE,SIG_IGN);

    // 创建线程池，初始化线程池
    threadpool<http_conn> * pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch(...) {
        exit(-1);
    }

    // 创建一个数组用于保存所有客户信息
    http_conn * users = new http_conn[ MAX_FD ];

    int listenfd = socket(PF_INET , SOCK_STREAM , 0);

    // 设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    // 监听
    listen(listenfd, 5);

    // 创建epoll对象，以及事件数组，添加监听
    epoll_event events[ MAX_EVENTS_NUMBER ];
    int epollfd = epoll_create(5);

    // 将监听的文件描述符添加
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
   
    setnonblocking( pipefd[1] );               // 写管道非阻塞
    addfd(epollfd, pipefd[0], false); // epoll检测读管道

    // 设置信号处理函数
    addsig(SIGALRM, sig_to_pipe);   // 定时器信号
    addsig(SIGTERM, sig_to_pipe);   // SIGTERM 关闭服务器
    bool stop_server = false;       // 关闭服务器标志位

    bool timeout = false;   // 定时器周期已到
    alarm(TIMESLOT);        // 定时产生SIGALRM信号

    printf("GO!\n");
    while(!stop_server) {
        int num = epoll_wait(epollfd, events, MAX_EVENTS_NUMBER, -1);
        if((num < 0) && (errno != EINTR)) {
            printf("epoll GG!");
            break;
        }

        for(int i = 0;i < num;i ++) {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd) {
                // 有客户端到来
                printf("some one in\n");
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd,(struct sockaddr*)&client_address, &client_addrlen);

                if(http_conn::m_user_count >= MAX_FD) {
                    // 目前连接数满了
                    // 给客户端写一个信息，服务器内部正忙。
                    close(connfd);
                    continue;
                }

                // 将新的客户数据初始化，放在数组中
                users[connfd].init(connfd, client_address);

            } // 读管道有数据，SIGALRM 或 SIGTERM信号触发
            else if(sockfd == pipefd[0] && (events[i].events & EPOLLIN)){  
                int sig;
                char signals[1024];
                int ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1){
                    continue;
                }else if(ret == 0){
                    continue;
                }else{
                    for(int i = 0; i < ret; ++i){
                        switch (signals[i]) // 字符ASCII码
                        {
                        case SIGALRM:
                        // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                        // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                            timeout = true;
                            break;
                        case SIGTERM:
                            stop_server = true;
                        }
                    }
                }
            } else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR) ) {
                // 对方发生错误
                users[sockfd].close_conn();
                http_conn::m_timer_lst.del_timer(users[sockfd].timer);  // 移除其对应的定时器
            } else if(events[i].events & EPOLLIN) {
                printf("EPOLLIN\n");
                if(users[sockfd].read()) {
                    // 一次性读完所有数据
                    pool->append(users + sockfd);
                    printf("finish doing,sockfd:%d\n",sockfd);
                } else {
                    users[sockfd].close_conn();
                    http_conn::m_timer_lst.del_timer(users[sockfd].timer);  // 移除其对应的定时器
                }
            } else if(events[i].events & EPOLLOUT) {
                printf("begin return\n");
                if(!users[sockfd].write()) { // 一次性写完所有数据
                    users[sockfd].close_conn();
                    http_conn::m_timer_lst.del_timer(users[sockfd].timer);  // 移除其对应的定时器
                }
            }
        }
        if(timeout) {
            // 定时处理任务，实际上就是调用tick()函数
            http_conn::m_timer_lst.tick();
            // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
            alarm(TIMESLOT);
            timeout = false;    // 重置timeout
        }
    }

    close(pipefd[1]);
    close(pipefd[0]);
    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;




    return 0;
}