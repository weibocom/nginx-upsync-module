#ifndef _NGX_HTTP_UPSYNC_MODELE_H_INCLUDED_
#define _NGX_HTTP_UPSYNC_MODELE_H_INCLUDED_


#include "ngx_http_json.h"
#include "ngx_http_parser.h"


#define ngx_strrchr(s1, c)              strrchr((const char *) s1, (int) c)
#define ngx_ftruncate(fd, offset)       ftruncate(fd, offset)
#define ngx_lseek(fd, offset, whence)   lseek(fd, offset, whence)


#define NGX_INDEX_HEARDER "X-Consul-Index"
#define NGX_INDEX_HEARDER_LEN 14

#define NGX_MAX_HEADERS 20
#define NGX_MAX_ELEMENT_SIZE 512

#define NGX_MAX_VALUE 65535

#define NGX_DELAY_DELETE 75 * 1000

#define NGX_ADD 0
#define NGX_DEL 1

#define NGX_PAGE_SIZE 4 * 1024
#define NGX_PAGE_NUMBER 1024

#define NGX_HTTP_RETRY_TIMES 3
#define NGX_HTTP_SOCKET_TIMEOUT 1


#if (NGX_HTTP_UPSTREAM_CHECK) 
extern ngx_module_t ngx_http_upstream_check_module;

typedef struct {
    ngx_uint_t                               type;

    ngx_str_t                                name;

    ngx_str_t                                default_send;

    /* HTTP */
    ngx_uint_t                               default_status_alive;

    ngx_event_handler_pt                     send_handler;
    ngx_event_handler_pt                     recv_handler;

    void *init;
    void *parse;
    void *reinit;

    unsigned need_pool;
    unsigned need_keepalive;
} ngx_check_conf_t;

typedef struct {
    ngx_uint_t                               port;
    ngx_uint_t                               fall_count;
    ngx_uint_t                               rise_count;
    ngx_msec_t                               check_interval;
    ngx_msec_t                               check_timeout;
    ngx_uint_t                               check_keepalive_requests;

    ngx_check_conf_t                        *check_type_conf;
    ngx_str_t                                send;

    union {
        ngx_uint_t                           return_code;
        ngx_uint_t                           status_alive;
    } code;

    ngx_array_t                             *fastcgi_params;

    ngx_uint_t                               default_down;
    ngx_uint_t                               unique;
} ngx_http_upstream_check_srv_conf_t;

extern ngx_uint_t ngx_http_upstream_check_add_dynamic_peer(ngx_pool_t *pool,
    ngx_http_upstream_srv_conf_t *us, ngx_addr_t *peer_addr);
extern void ngx_http_upstream_check_delete_dynamic_peer(ngx_str_t *name,
    ngx_addr_t *peer_addr);
#endif


#endif //_NGX_HTTP_UPSYNC_MODELE_H_INCLUDED_
