#ifndef _PT_ENGINE_CLIENT_H_
#define _PT_ENGINE_CLIENT_H_

#include "packet.h"
#include "buffer.h"

struct pt_client;


typedef void (*pt_cli_on_connected)(struct pt_client *conn);
typedef void (*pt_cli_on_receive)(struct pt_client *conn, struct pt_buffer *buff);
typedef void (*pt_cli_on_disconnected)(struct pt_client *conn);
struct pt_client
{
    //socket
    uv_tcp_t conn;
    
    //uv_loop 主循环
    uv_loop_t *loop;
    
    //成功连接服务器后调用
    pt_cli_on_connected on_connected;
    
    //数据到达后调用
    pt_cli_on_receive on_receive;
    
    //断开连接后调用
    pt_cli_on_disconnected on_disconnected;
    
    //是否已经建立了连接
    qboolean connected;
    
    //正在连接中
    qboolean connecting;
    
    //收到的缓冲区数据
    struct pt_buffer *buf;
    
    //投递给libuv的异步缓冲区
    uv_buf_t *async_buf;
    
    //时间戳
    uint32_t timestamp;
    
    
    /*
     提供给libuv的回调函数
     */
    uv_write_cb write_cb;
    uv_read_cb read_cb;
    uv_connect_cb connect_cb;
    uv_shutdown_cb shutdown_cb;
};

struct pt_client *pt_client_new();
void pt_client_init(uv_loop_t *loop, struct pt_client *client, pt_cli_on_connected on_connected, pt_cli_on_receive on_receive, pt_cli_on_disconnected on_disconnected);
void pt_client_free(struct pt_client *client);
void pt_client_send(struct pt_client *client, struct pt_buffer *buff);
void pt_client_connect(struct pt_client *client, const char *host, uint16_t port);
void pt_client_disconnect(struct pt_client *client);


#endif
