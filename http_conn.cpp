#include "http_conn.h"

int http_conn::m_epollfd = -1;  // 注册到同一个
int http_conn::m_user_count = 0; // 用户数量

const char * doc_root = "/home/dai/webserver/resource";
const int FILENAME_LEN = 29;

const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";


void setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    old_flag |= O_NONBLOCK;
    fcntl(fd, F_SETFL, old_flag);
}

void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;

    if(one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置阻塞与否
    setnonblocking(fd);
}

void removefd(int epollfd,int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 重置SHOT
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void http_conn::init(int sockfd,const sockaddr_in & addr) {
    m_sockfd = sockfd;
    m_address = addr;

    // 设置端口复用

    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //添加到epoll里面
    addfd(m_epollfd, m_sockfd, true);
    m_user_count ++;

    init();

}

void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始化为解析首行
    m_check_index = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = false;
    m_content_length = 0;
    m_file_address = 0;
    m_write_idx = 0;                // 写缓冲区中待发送的字节数
    m_iv_count = 0;                 // 被写内存块的数量
    bytes_to_send = 0;              // 将要发送的字节
    bytes_have_send = 0;            // 已经发送的字节
    /*
    int m_sockfd; // 该HTTP连接的fd
    sockaddr_in m_address; // 地址
    int m_read_idx; // 最后一个字节的下标
    int m_check_index; // 当前的字符的位置
    int m_start_line; // 当前解析的行的起始位置
    char * m_url;
    char * m_version;
    METHOD m_method;
    char * m_host;
    bool m_linger;
    int m_content_length;
    struct stat m_file_state;
    char * m_file_address;
    int m_write_idx;                // 写缓冲区中待发送的字节数
    int m_iv_count;                 // 被写内存块的数量
    int bytes_to_send;              // 将要发送的字节
    int bytes_have_send;            // 已经发送的字节
    */



    

    bzero(m_read_buf, READ_BUFFER_SIZE);
}

// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count --;
    } 
}

// 循环读取客户数据，直到无数据到达或者对方关闭数据
bool http_conn::read(){
    
    if(m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    // 已经读取到的字节
    int bytes_read = 0;
    while(true) {
        // 从已经写的了地方向后延长地写
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有数据
                break;
            }
            return false;
        } else if(bytes_read == 0) {
            // 对方关闭连接
            return false;
        } 
        m_read_idx += bytes_read;
    }
    printf("read all:%s\n", m_read_buf);
    return true;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char * text = 0;

    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) 
        || ((line_status = parse_line()) == LINE_OK)) {
        // 解析到了一行完整的数据，或者解析到了请求体
        
        // 获取到了一行数据
        text = get_line();

        m_start_line = m_check_index;
        printf("got 1 line:%s\n",text);

        switch (m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:{
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST)
                return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER:{
                ret = parse_headers(text);
                if(ret == BAD_REQUEST)
                return BAD_REQUEST;
                else if(ret == GET_REQUEST) {
                    printf("在case CHECK_STATE_HEADER:2情况跳出\n");
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:{
                ret = parse_headers(text);
                if(ret == GET_REQUEST)
                return do_request();
                line_status = LINE_OPEN;
                break;
            }
            default:{
                return INTERNAL_ERROR;
            }
        }            

    }

    return NO_REQUEST;
} 

http_conn::HTTP_CODE http_conn::parse_request_line(char * text){
    m_url = strpbrk(text, " \t");
    *m_url++ = '\0';
    char * method = text;
    if(strcasecmp(method,"GET") == 0) {
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    m_version = strpbrk(m_url, " \t");
    if(!m_version) return BAD_REQUEST;
    *m_version++ = '\0';
    if(strcasecmp(m_version,"HTTP/1.1") != 0) {
        return BAD_REQUEST;
    } 

    if((strncasecmp(m_url,"http://",7)) == 0) {
        m_url += 7;
        m_url = strchr(m_url,'/');
    }

    if(!m_url || m_url[0] != '/')
    return BAD_REQUEST;

    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
} 

http_conn::HTTP_CODE http_conn::parse_headers(char * text){
    if(text[0] == '\0') {
       if(m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
       }
       printf("return GET_REQUEST\n");
       return GET_REQUEST;
    } else if(strncasecmp(text,"Connection:",11) == 0) {
        text += 11;
        text += strspn(text,"\t");
        if(strcasecmp(text,"keep-alive") == 0) {
            m_linger = true;
        }
    } else if(strncasecmp(text,"Content-Length:",15) == 0){
        text += 15;
        text += strspn(text,"\t");
        m_content_length = atol(text);
    } else if(strncasecmp(text,"Host:",5) == 0){
        text += 5;
        text += strspn(text,"\t");
        m_host = text;
    } else {
        printf("oop!\n");
    }
    return NO_REQUEST;
    
} 

http_conn::HTTP_CODE http_conn::parse_content(char * text){
    if ( m_read_idx >= ( m_content_length + m_check_index ) )    // 读到的数据长度 大于 已解析长度（请求行+头部+空行）+请求体长度
    {                                                       // 数据被完整读取
        text[ m_content_length ] = '\0';   // 标志结束
        return GET_REQUEST;
    }
    return NO_REQUEST;
} 

http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    for(;m_check_index < m_read_idx;++m_check_index) {
        temp = m_read_buf[m_check_index];
        if( temp == '\r') {
            if((m_check_index+1) == m_read_idx) {
                return LINE_OPEN;
            } else if(m_read_buf[m_check_index+1] == '\n') {
                m_read_buf[m_check_index++] = '\0';
                m_read_buf[m_check_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if(temp == '\n') {
            if((m_check_index > 1) && (m_read_buf[m_check_index-1] == '\r')) {
                m_read_buf[m_check_index-1] = '\0';
                m_read_buf[m_check_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;   // 没有到结束符，数据尚不完整
}

http_conn::HTTP_CODE http_conn::do_request() {
    printf("do_request\n");
    strcpy( m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len,m_url,strlen(m_url));
    printf("m_real_file:%s\n",m_real_file);
    printf("m_url:%s\n",m_url);
    if(stat(m_real_file,&m_file_state) < 0) {
        printf("1\n");
        return NO_RESOURCE;
    }
    if(!(m_file_state.st_mode & S_IROTH)) {
        printf("2\n");
        return FORBINDDEN_REQUEST;
    }
    if(S_ISDIR(m_file_state.st_mode)) {
        printf("3\n");
        return BAD_REQUEST;
    }

    int fd = open(m_real_file,O_RDONLY);
    m_file_address = (char*)mmap(0,m_file_state.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    printf("return FILE_REQUEST\n");
    return FILE_REQUEST;
}
    
void http_conn::unmap() {
    printf("unmap is doing\n");
    if(m_file_address) {
        munmap(m_file_address,m_file_state.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write() {
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写   m_write_buf + m_file_address
        int temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();        // 释放内存映射m_file_address空间
            return false;
        }

        bytes_to_send -= temp;
        bytes_have_send += temp;


        if (bytes_to_send <= 0){
            // 没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger){
                init();
                return true;
            }else{
                return false;
            }
        }
    }
}

bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WD_BUF_SIZE ) {      // 写缓冲区满了
        return false;
    }
    va_list arg_list;                       // 可变参数，格式化文本
    va_start( arg_list, format );           // 添加文本到到写缓冲区m_write_buf中
    int len = vsnprintf( m_write_buf + m_write_idx, WD_BUF_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WD_BUF_SIZE - 1 - m_write_idx ) ) {
        return false;                       // 没写完，已经满了
    }
    m_write_idx += len;                     // 更新下次写数据的起始位置
    va_end( arg_list );
    return true;
}
// 添加状态码（响应行）
bool http_conn::add_status_line( int status, const char* title ) {  
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

// 添加了一些必要的响应头部
void http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}
bool http_conn::add_content_type() {    // 响应体类型，当前文本形式
    return add_response("Content-Type:%s\r\n", "text/html");    
}
bool http_conn::add_linger(){
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}
bool http_conn::add_blank_line(){  
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content ){
    return add_response( "%s", content );
}

bool http_conn::process_write(HTTP_CODE ret) {
    printf(" \n is process_write,ret:%d \n",ret);

       switch (ret)
    {
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:  // 请求文件
            printf("请求文件\n");
            add_status_line(200, ok_200_title );
            add_headers(m_file_state.st_size);
            // 封装m_iv
            m_iv[ 0 ].iov_base = m_write_buf;   // 起始地址
            m_iv[ 0 ].iov_len = m_write_idx;    // 长度
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_state.st_size;
            m_iv_count = 2;                     // 两块内存
            bytes_to_send = m_write_idx + m_file_state.st_size;  // 响应头的大小 + 文件的大小
            return true;
        default:
            printf("default\n");
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    printf("此时没有任何东西\n");
    return true;
}




// 由线程池里面的工作线程调用，处理HTTP线程
void http_conn::process() {

    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST) {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }

    printf("parse request,create response\n");
    //NO_REQUEST,GET_REQUEST,BAD_REQUEST,NO_RESOURCE,FORBINDDEN_REQUEST,INTERNAL_ERROR,FILE_REQUEST

    printf("read_ret:%d\n",read_ret);

    // 生成响应
    bool write_ret = process_write(read_ret);
    if(!write_ret) close_conn();

    modfd(m_epollfd,m_sockfd,EPOLLOUT);

}


// GET / HTTP/1.1
// Host: 192.168.59.1:9999
// Connection: keep-alive
// Upgrade-Insecure-Requests: 1
// User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/98.0.4758.82 Safari/537.36
// Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.9
// Accept-Encoding: gzip, deflate
// Accept-Language: zh-CN,zh;q=0.9


