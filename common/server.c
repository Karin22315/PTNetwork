//
//  server.c
//  xcode
//
//  Created by 袁睿 on 16/6/22.
//  Copyright © 2016年 number201724. All rights reserved.
//

#include "common.h"
#include "error.h"
#include "crc32.h"
#include "server.h"

static void pt_server_log(const char *fmt, int error, const char *func, const char *file, int line)
{
    char log[512];
    sprintf(log, fmt, uv_strerror(error));
    LOG(log,func,file,line);
}

static void pt_server_alloc_buf(uv_handle_t* handle,size_t suggested_size,uv_buf_t* buf) {
    uv_stream_t *sock = (uv_stream_t*)handle;
    struct pt_sclient *user = sock->data;
    
    if(user->async_buf){
        if(user->async_buf->len < suggested_size){
            free(user->async_buf->base);
            user->async_buf->len = suggested_size;
            user->async_buf->base = malloc(suggested_size);
            if(user->async_buf->base == NULL){
                FATAL("malloc user->readbuf->buf failed", __FUNCTION__, __FILE__, __LINE__);
                abort();
            }
        }
    } else {
        user->async_buf = malloc(sizeof(*user->async_buf));
        user->async_buf->base = malloc(suggested_size);
        user->async_buf->len = suggested_size;
        if(user->async_buf->base == NULL){
            FATAL("malloc user->readbuf->buf failed", __FUNCTION__, __FILE__, __LINE__);
            abort();
        }
    }
    
    
    buf->base = (char*)user->async_buf->base;
    buf->len = user->async_buf->len;
}

static struct pt_sclient* pt_sclient_new(struct pt_server *server)
{
    struct pt_sclient *user;
    
    user = malloc(sizeof(struct pt_sclient));
    bzero(user, sizeof(struct pt_sclient));
    
    user->buf = pt_buffer_new(USER_DEFAULT_BUFF_SIZE);
    user->id = ++server->serial;
    
    return user;
}

static void pt_sclient_free(struct pt_sclient* user)
{
    pt_buffer_free(user->buf);
    user->buf = NULL;
    
    if(user->async_buf){
        free(user->async_buf->base);
        free(user->async_buf);
        user->async_buf = NULL;
    }
    free(user);
}

/*
 当写入操作完成或失败会执行本函数
 释放pt_buffer和write_request数据
 */
static void pt_server_write_cb(uv_write_t* req, int status)
{
    struct pt_wreq *wr = (struct pt_wreq*)req;
    
    pt_buffer_free(wr->buff);
    free(wr);
}

/*
 当成功关闭了handle后，释放用户资源
 */
static void pt_server_on_close_conn(uv_handle_t* peer) {
    struct pt_sclient *user = peer->data;
    
    pt_sclient_free(user);
}


//关闭一个客户端的连接
static void pt_server_close_conn(struct pt_sclient *user, qboolean remove)
{
    struct pt_server *server = user->server;
    
    //检查用户是否已被关闭
    if(user->connected  == false)
    {
        return;
    }
    
    user->connected = false;
    
    if(remove)
    {
        //通知用户函数，用户断开
        if(server->on_disconnect) server->on_disconnect(user);
        
        //从用户ID表中删除
        pt_table_erase(server->clients, user->id);
        
        //降低服务器连接数
        server->number_of_connected--;
    }
    
    uv_close((uv_handle_t*)&user->sock.stream, pt_server_on_close_conn);
}


//关闭服务器的回调函数
static void pt_server_on_close_listener(uv_handle_t *handle)
{
    struct pt_server *server = handle->data;
    server->is_startup = false;
}

/*
 libuv的数据包收到函数
 
 当用户收到数据或连接断开时，会调用本函数
 本函数会处理用户断开
 数据到达
 对数据安全进行检查等
 */
static void pt_server_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    uint32_t packet_err = PACKET_INFO_OK;   //默认是没有任何错误的
    struct pt_sclient *user = stream->data;
    struct pt_buffer *userbuf = NULL;
    
    //用户状态异常断开，执行disconnect
    if(nread < 0)
    {
        DBGPRINT("nread < 0");
        pt_server_close_conn(user, true);
        return;
    }
    
    //用户发送了EOF包，执行断开
    if(nread == 0){
        DBGPRINT("nread == 0");
        pt_server_close_conn(user, true);
        return;
    }
    
    //将数据追加到缓冲区
    pt_buffer_write(user->buf, (unsigned char*)buf->base, (uint32_t)nread);
    
    //循环读取缓冲区数据，如果数据错误则返回false且不再执行本while
    //稳定性修复，当客户端断开的时候，不再处理接收的数据
    //等待系统的回收
    //在这里connected == false一般是由pt_server_send函数overflow导致的
    while(user->connected && pt_get_packet_status(user->buf, &packet_err))
    {
        //拆分一个数据包
        userbuf = pt_split_packet(user->buf);
        if(userbuf != NULL)
        {
            //如果服务器开启了加密功能,则执行解密函数
            if(user->server->enable_encrypt)
            {
                //对数据包进行解密，并且校验包序列，且校验数据的crc是否正确
                if(pt_decrypt_package(user->serial, &user->encrypt_ctx, userbuf) == false)
                {
                    //数据不正确，断开用户的连接，释放缓冲区
                    pt_buffer_free(userbuf);
                    pt_server_close_conn(user, true);
                    return;
                }
                
                //数据正确，增加包序列
                user->serial++;
            }
            
            //回调用户函数，通知数据到达
            if(user->server->on_receive)
            {
                user->server->on_receive(user, userbuf);
            }
            
            //释放拆分包后的数据
            pt_buffer_free(userbuf);
        }
        else
        {
            //一般情况下不会出现拆包等于0的问题，如果出现这个则肯定是致命错误
            FATAL("pt_split_packet == NULL wtf?", __FUNCTION__, __FILE__, __LINE__);
        }
    }
    
    //如果用户发的数据是致命错误，则干掉用户
    if(packet_err == PACKET_INFO_FAKE || packet_err == PACKET_INFO_OVERFLOW){
        char error[255];
        const char *msg = packet_err == PACKET_INFO_FAKE ? "PACKET_INFO_FAKE" : "PACKET_INFO_OVERFLOW";
        sprintf(error, "pt_get_packet_status error:%s",msg);
        ERROR(error, __FUNCTION__, __FILE__, __LINE__);
        pt_server_close_conn(user, true);
    }
}

/*
 libuv的connection通知
 
 处理新用户连接，最大用户数量，创建pt_sclient结构并添加到clients表中
 执行用户自定义的通知信息
 */
static void pt_server_connection_cb(uv_stream_t* listener, int status)
{
    int r;
    struct pt_server *server = listener->data;
    struct pt_sclient *user = pt_sclient_new(server);
    
    user->server = server;
    
    if(server->is_pipe){
        uv_pipe_init(listener->loop, &user->sock.pipe, true);
    } else {
        uv_tcp_init(listener->loop, &user->sock.tcp);
    }
    
    user->sock.stream.data = user;
    
    r = uv_accept(listener, (uv_stream_t*)&user->sock);
    
    //如果accept失败，写出错误日志，并关掉这个sock
    if( r != 0 )
    {
        char error[255];
        sprintf(error, "uv_accept error:%s",uv_strerror(r));
        ERROR(error, __FUNCTION__, __FILE__, __LINE__);
        uv_close((uv_handle_t*)&user->sock, pt_server_on_close_conn);
        return;
    }
    
    //设置客户端连接已经成功
    user->connected = true;
    
    //限制当前服务器的最大连接数
    if(server->number_of_connected + 1 > server->number_of_max_connected){
        pt_server_close_conn(user, false);
        return;
    }
    
    //进行数据过滤，如果用户Connect请求被on_connect干掉则直接断开用户
    if(server->on_connect && server->on_connect(user) == false){
        pt_server_close_conn(user, false);
        return;
    }
    
    if(server->enable_encrypt){
        user->serial = 0;
        RC4_set_key(&user->encrypt_ctx, sizeof(server->encrypt_key), (const unsigned char*)&server->encrypt_key);
    }
    
    server->number_of_connected++;
    
    if(server->is_pipe == false){
        //设置用户30秒后做keepalive检查
        uv_tcp_keepalive(&user->sock.tcp, true, server->keep_alive_delay);
        
        
        if(server->no_delay){
            uv_tcp_nodelay(&user->sock.tcp, true);
        }
    }
    
    //添加到搜索树内
    pt_table_insert(server->clients, user->id, user);
    
    //开始读取网络数据
    r = uv_read_start(&user->sock.stream, pt_server_alloc_buf, server->read_cb);
    
    if( r != 0 )
    {
        char error[255];
        sprintf(error, "uv_read_start error:%s",uv_strerror(r));
        ERROR(error, __FUNCTION__, __FILE__, __LINE__);
        
        //修复一个bug，之前直接使用uv_close，正确应该使用这个函数
        pt_server_close_conn(user, true);
        return;
    }
}

struct pt_server* pt_server_new()
{
    struct pt_server *server = (struct pt_server *)malloc(sizeof(struct pt_server));
    
    if(server == NULL){
        FATAL("malloc pt_server failed",__FUNCTION__,__FILE__,__LINE__);
        abort();
    }
    
    bzero(server, sizeof(struct pt_server));
    
    server->clients = pt_table_new();
    
    //初始化libuv的回调函数
    server->connection_cb = pt_server_connection_cb;
    server->read_cb = pt_server_read_cb;
    server->write_cb = pt_server_write_cb;
    server->number_of_max_send_queue = 1000;
    
    return server;
}



void pt_server_free(struct pt_server *srv)
{
    if(srv->is_startup == true){
        FATAL("server not shutdown", __FUNCTION__, __FILE__, __LINE__);
        abort();
    }
    pt_table_free(srv->clients);
    
    free(srv);
}

void pt_server_set_nodelay(struct pt_server *server, qboolean nodelay)
{
    server->no_delay = nodelay;
}

void pt_server_init(struct pt_server *server, uv_loop_t *loop, int max_conn, int keep_alive_delay,pt_server_on_connect on_conn,
                        pt_server_on_receive on_receive, pt_server_on_disconnect on_disconnect)
{
    if(server->is_init){
        LOG("server already initialize",__FUNCTION__,__FILE__,__LINE__);
        return;
    }
    
    server->number_of_max_connected = max_conn;
    server->loop = loop;
    server->on_connect = on_conn;
    server->on_receive = on_receive;
    server->on_disconnect = on_disconnect;
    server->keep_alive_delay = keep_alive_delay;
    
    server->is_init = true;
}

qboolean pt_server_start(struct pt_server *server, const char* host, uint16_t port)
{
    int r;
    struct sockaddr_in adr;
    
    
    if(!server->is_init){
        LOG("server not initialize",__FUNCTION__,__FILE__,__LINE__);
        return false;
    }
    
    r = uv_tcp_init(server->loop, &server->listener.tcp);
    if(r != 0){
        char log[256];
        sprintf(log,"uv_tcp_init failed:%s",uv_strerror(r));
        LOG(log,__FUNCTION__, __FILE__,__LINE__);
    }
    
    server->is_pipe = false;
    server->listener.stream.data = server;
    
    uv_ip4_addr(host, port, &adr);
    
    r = uv_tcp_bind(&server->listener.tcp, (const struct sockaddr*)&adr, 0);
    
    if(r != 0){
        pt_server_log("uv_tcp_bind failed:%s",r, __FUNCTION__, __FILE__, __LINE__);
        return false;
    }
    
    r = uv_listen((uv_stream_t*)&server->listener, SOMAXCONN, server->connection_cb);
    if(r != 0){
        pt_server_log("uv_listen failed:%s",r, __FUNCTION__, __FILE__, __LINE__);
        return false;
    }
    
    server->is_startup = true;
    return true;
}


qboolean pt_server_start_pipe(struct pt_server *server, const char *path)
{
    int r;
    
    if(!server->is_init){
        LOG("server not initialize",__FUNCTION__,__FILE__,__LINE__);
        return false;
    }
    
    r = uv_pipe_init(server->loop, &server->listener.pipe, true);
    if(r != 0){
        char log[256];
        sprintf(log,"uv_tcp_init failed:%s",uv_strerror(r));
        LOG(log,__FUNCTION__, __FILE__,__LINE__);
    }
    
    server->is_pipe = true;
    server->listener.stream.data = server;
    
    remove(path);
    r = uv_pipe_bind(&server->listener.pipe, path);
    if(r != 0){
        pt_server_log("uv_tcp_bind failed:%s",r, __FUNCTION__, __FILE__, __LINE__);
        return false;
    }
    
    r = uv_listen(&server->listener.stream, SOMAXCONN, server->connection_cb);
    if(r != 0){
        pt_server_log("uv_listen failed:%s",r, __FUNCTION__, __FILE__, __LINE__);
        return false;
    }
    
    server->is_startup = true;
    return true;
}



void pt_server_set_encrypt(struct pt_server *server, const uint32_t encrypt_key[4])
{
    server->enable_encrypt = true;
    
    for(int i =0;i<4;i++)
    {
        server->encrypt_key[i] = encrypt_key[i];
    }
}

/*
 发送数据到客户端
 */
qboolean pt_server_send(struct pt_sclient *user, struct pt_buffer *buff)
{
    if(user->connected == false){
        pt_buffer_free(buff);
        return false;
    }
    
    //防止服务器发包过多导致服务器的内存耗尽
    if(user->sock.stream.write_queue_size >= user->server->number_of_max_send_queue){
        DBGPRINT("user datagram overflow");
        pt_server_close_conn(user, true);
        pt_buffer_free(buff);
        return false;
    }
    
    struct pt_wreq *wreq = malloc(sizeof(struct pt_wreq));
    
    wreq->buff = buff;
    wreq->buf = uv_buf_init((char*)buff->buff, buff->length);
    
    if (uv_write(&wreq->req, (uv_stream_t*)&user->sock, &wreq->buf, 1, user->server->write_cb)) {
        pt_buffer_free(wreq->buff);
        free(wreq);
        return false;
    }
    
    return true;
}


void pt_server_close(struct pt_server *server)
{
    uint32_t i;
    struct pt_table_node *n;
    struct pt_table_node *p;
    for(i = 0; i < server->clients->granularity;i++)
    {
        n = server->clients->head[i];
        while(n){
            p = n;
            n = n->next;
            pt_server_close_conn(p->ptr, true);
        }
    }
    uv_close((uv_handle_t*)&server->listener, pt_server_on_close_listener);
}

qboolean pt_server_disconnect_conn(struct pt_sclient *user)
{
    if(user->connected){
        pt_server_close_conn(user, true);
        return true;
    }
    return false;
}