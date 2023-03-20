#ifndef _HTTP_CONN_H_
#define _HTTP_CONN_H_

#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>
#include <stdarg.h>

// 工作队列的具体工作
class http_conn {
public:

    static int m_epollfd;  // 注册到同一个
    static int m_user_count; // 用户数量
    static const int READ_BUFFER_SIZE = 2048;
    static const int WD_BUF_SIZE = 2048;

    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTION, CONNECT};
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0,CHECK_STATE_HEADER,CHECK_STATE_CONTENT};
    enum LINE_STATUS {LINE_OK = 0,LINE_BAD,LINE_OPEN};
    enum HTTP_CODE {NO_REQUEST,GET_REQUEST,BAD_REQUEST,NO_RESOURCE,FORBINDDEN_REQUEST,INTERNAL_ERROR,FILE_REQUEST};

    http_conn() {}
    ~http_conn() {}

    // 处理客户端的请求
    void process();
    void init(int sockfd,const sockaddr_in & addr);
    void close_conn();
    bool read();  // 非阻塞
    bool write(); // 非阻塞

    HTTP_CODE process_read(); // 解析HTTP请求
    HTTP_CODE parse_request_line(char * text); // 解析HTTP首行
    HTTP_CODE parse_headers(char * text); // 解析HTTP头
    HTTP_CODE parse_content(char * text); // 解析HTTP内容
    LINE_STATUS parse_line(); // 解析行
    char * get_line(){return m_read_buf + m_start_line;}
    HTTP_CODE do_request();
    void unmap();
    bool process_write(HTTP_CODE ret);

    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    void add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line(); 
    
private:

    int m_sockfd; // 该HTTP连接的fd
    sockaddr_in m_address; // 地址
    char m_read_buf[READ_BUFFER_SIZE];
    int m_read_idx; // 最后一个字节的下标
    int m_check_index; // 当前的字符的位置
    int m_start_line; // 当前解析的行的起始位置
    char * m_url;
    char * m_version;
    METHOD m_method;
    char * m_host;
    bool m_linger;
    int m_content_length;
    char m_real_file[200];
    struct stat m_file_state;
    char * m_file_address;

    CHECK_STATE m_check_state; // 主状态机所处位置
    void init(); // 初始化连接部分

    char m_write_buf[WD_BUF_SIZE];  // 写缓冲区
    int m_write_idx;                // 写缓冲区中待发送的字节数
    struct iovec m_iv[2];           // writev来执行写操作，表示分散写两个不连续内存块的内容
    int m_iv_count;                 // 被写内存块的数量
    int bytes_to_send;              // 将要发送的字节
    int bytes_have_send;            // 已经发送的字节

};


#endif