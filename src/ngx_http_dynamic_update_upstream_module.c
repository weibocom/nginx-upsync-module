/*
 * Copyright (C) 2015 Weibo Group Holding Limited
 * Copyright (C) 2015 Xiaokai Wang (xiaokai.wang@live.com)
 */


#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_config.h>

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

#define NGX_DELAY_DELETE 150 * 1000

#define NGX_ADD 0
#define NGX_DEL 1

#define NGX_BLOCK 1024
#define NGX_PAGESIZE 4 * 1024
#define NGX_PAGE_COUNT 1024

#define NGX_BACKEND_NUMBER   35  /* everypage(4K) can store backend number is 35 approximately*/

#define NGX_HTTP_RETRY_TIMES 3
#define NGX_HTTP_SOCKET_TIMEOUT 1


/* client for conf service */
typedef struct {
    int                 sd;
    int                 port;
    char                ip[16];
    struct sockaddr_in  addr;
    int                 connected;
} ngx_http_conf_client;


typedef struct {
    u_char                           sockaddr[NGX_SOCKADDRLEN];

    ngx_int_t                        weight;
    ngx_uint_t                       max_fails;
    time_t                           fail_timeout;

    unsigned                         down:1;
    unsigned                         backup:1;
} ngx_http_update_conf_t;


typedef struct {
    ngx_pool_t                      *pool;

    ngx_buf_t                        send;
    ngx_buf_t                        recv;

    ngx_buf_t                        body;

    ngx_uint_t                       upstream_count;

    ngx_array_t                      del_upstream;  /* ngx_http_update_conf_t */
    ngx_array_t                      add_upstream;

    ngx_array_t                      upstream_conf;
} ngx_http_dynamic_update_upstream_ctx_t;


typedef struct {
    ngx_str_t            consul_host;
    ngx_int_t            consul_port;

    ngx_msec_t           update_timeout;
    ngx_msec_t           update_interval;

    ngx_msec_t           delay_delete;

    ngx_uint_t           strong_dependency;

    ngx_str_t            update_send;
    ngx_str_t            upstream_conf_path;

    ngx_open_file_t     *conf_file;

    ngx_http_upstream_server_t    us;         //consul server
} ngx_http_dynamic_update_upstream_srv_conf_t;


/* based on upstream conf, every unit update from consul */
typedef struct {
    ngx_event_t                              update_ev;
    ngx_event_t                              update_timeout_ev;

    ngx_queue_t                              add_ev;
    ngx_queue_t                              delete_ev;

    ngx_int_t                                index;

    ngx_str_t                                host;

    ngx_shmtx_t                              dynamic_accept_mutex;

    ngx_peer_connection_t                    pc;

    ngx_event_handler_pt                     send_handler;
    ngx_event_handler_pt                     recv_handler;

    ngx_http_upstream_srv_conf_t                 *uscf;

    ngx_http_dynamic_update_upstream_ctx_t        ctx;

    ngx_http_dynamic_update_upstream_srv_conf_t  *conf;
} ngx_http_dynamic_update_upstream_server_t;


typedef struct {
    ngx_event_t                              delay_delete_ev;

    ngx_queue_t                              queue;

    time_t                                   start_sec;
    ngx_msec_t                               start_msec;

    void                                    *data;
} ngx_delay_event_t;


typedef struct {
    ngx_uint_t                                  upstream_num;

    ngx_http_dynamic_update_upstream_server_t  *conf_server;
} ngx_http_dynamic_update_upstream_main_conf_t;


typedef struct {
    u_char     status[3];

    char       headers[NGX_MAX_HEADERS][2][NGX_MAX_ELEMENT_SIZE];

    ngx_uint_t num_headers;

    enum { NONE=0, FIELD, VALUE } last_header;

    u_char     http_body[NGX_PAGESIZE * NGX_BLOCK];
} ngx_http_state;


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

static char *ngx_http_dynamic_update_upstream_consul_server(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf);
static char *ngx_http_dynamic_update_upstream_set_conf_dump(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf);

static void *ngx_http_dynamic_update_upstream_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_dynamic_update_upstream_create_srv_conf(ngx_conf_t *cf);
static char *ngx_http_dynamic_update_upstream_init_main_conf(ngx_conf_t *cf, void *conf);
static char *ngx_http_dynamic_update_upstream_init_srv_conf(ngx_conf_t *cf, void *conf, 
        ngx_uint_t num);

static void ngx_http_dynamic_update_upstream_process(
        ngx_http_dynamic_update_upstream_server_t *conf_server);

static ngx_int_t ngx_http_dynamic_update_upstream_init_process(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_dynamic_update_upstream_init_module(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_dynamic_update_upstream_init_shm_mutex(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_dynamic_update_upstream_add_timers(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_dynamic_update_upstream_init_peers(ngx_cycle_t *cycle,
        ngx_http_dynamic_update_upstream_server_t *conf_server);

static void ngx_http_dynamic_update_upstream_begin_handler(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_connect_handler(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_recv_handler(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_send_handler(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_timeout_handler(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_clean_event(
        ngx_http_dynamic_update_upstream_server_t *peer);
static ngx_int_t ngx_http_dynamic_update_upstream_dump_conf(
        ngx_http_dynamic_update_upstream_server_t *conf_server);
static ngx_int_t ngx_http_dynamic_update_upstream_init_consul(ngx_event_t *event);

static ngx_int_t ngx_http_dynamic_update_upstream_add_server(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server);
static ngx_int_t ngx_http_dynamic_update_upstream_add_peer(ngx_cycle_t *cycle, 
        ngx_array_t *servers, ngx_http_dynamic_update_upstream_server_t *conf_server);
static void ngx_http_dynamic_update_upstream_add_check(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server);

static ngx_int_t ngx_http_dynamic_update_upstream_del_server(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server);
static ngx_int_t ngx_http_dynamic_update_upstream_del_peer(ngx_cycle_t *cycle,
        ngx_http_upstream_server_t *us, ngx_http_dynamic_update_upstream_server_t *conf_server);
static void ngx_http_dynamic_update_upstream_del_check(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server);

static ngx_int_t ngx_http_dynamic_update_upstream_server_weight(ngx_cycle_t *cycle,
        ngx_http_dynamic_update_upstream_server_t *conf_server);

static void ngx_http_dynamic_update_upstream_event_init(ngx_http_upstream_rr_peers_t *tmp_peers, 
        ngx_http_dynamic_update_upstream_server_t *conf_server, ngx_flag_t flag);

static ngx_int_t ngx_http_parser_init();
static void ngx_http_parser_execute(ngx_http_dynamic_update_upstream_ctx_t *ctx);

int ngx_http_status(http_parser *p, const char *buf, size_t len);
int ngx_http_header_field_cb(http_parser *p, const char *buf, size_t len);
int ngx_http_header_value_cb(http_parser *p, const char *buf, size_t len);
int ngx_http_body(http_parser *p, const char *buf, size_t len);

static ngx_int_t ngx_http_dynamic_update_upstream_parse_json(u_char *buf, 
        ngx_http_dynamic_update_upstream_server_t *conf_server);
static ngx_int_t ngx_http_dynamic_update_upstream_check_key(u_char *key);
static void ngx_http_dynamic_update_upstream_free(ngx_http_upstream_rr_peers_t *peers);

static void ngx_http_dynamic_update_upstream_add_delay_delete(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_del_delay_delete(ngx_event_t *event);

static ngx_int_t ngx_http_dynamic_update_upstream_need_exit();
static void ngx_http_dynamic_update_upstream_clear_all_events();

static ngx_int_t ngx_http_dynamic_update_upstream_get_all(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server, char **conf_value);
static ngx_http_conf_client *ngx_http_create_client(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server);
static ngx_int_t ngx_http_client_conn(ngx_http_conf_client *client);
static void ngx_http_client_destroy(ngx_http_conf_client *client);
static ngx_int_t ngx_http_client_send(ngx_http_conf_client *client, 
        ngx_http_dynamic_update_upstream_server_t *conf_server);
static ngx_int_t ngx_http_client_recv(ngx_http_conf_client *client, char **data, int size);

static u_char *ngx_print_escape(u_char *dst, u_char *src, size_t len);

static char *ngx_http_dynamic_update_upstream_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_dynamic_update_upstream_show(ngx_http_request_t *r);


static http_parser_settings settings = {
    .on_message_begin = 0,
    .on_header_field = ngx_http_header_field_cb,
    .on_header_value = ngx_http_header_value_cb,
    .on_url = 0,
    .on_status = ngx_http_status,
    .on_body = ngx_http_body,
    .on_headers_complete = 0,
    .on_message_complete = 0
};

ngx_atomic_t   dynamic_shared_created0;
ngx_atomic_t  *dynamic_shared_created = &dynamic_shared_created0;

static http_parser *parser = NULL;
static ngx_http_state state;

static ngx_http_dynamic_update_upstream_main_conf_t  *dynamic_upstream_ctx = NULL;

static ngx_command_t  ngx_http_dynamic_update_upstream_commands[] = {

    {  ngx_string("consul"),
        NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
        ngx_http_dynamic_update_upstream_consul_server,
        NGX_HTTP_SRV_CONF_OFFSET,
        0,
        NULL },

    {  ngx_string("upstream_conf_path"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_http_dynamic_update_upstream_set_conf_dump,
        NGX_HTTP_SRV_CONF_OFFSET,
        0,
        NULL },

    {  ngx_string("upstream_show"),
        NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
        ngx_http_dynamic_update_upstream_set,
        0,
        0,
        NULL },

    ngx_null_command
};


static ngx_http_module_t  ngx_http_dynamic_update_upstream_module_ctx = {
    NULL,                                              /* preconfiguration */
    NULL,                                              /* postconfiguration */

    ngx_http_dynamic_update_upstream_create_main_conf, /* create main configuration */
    ngx_http_dynamic_update_upstream_init_main_conf,   /* init main configuration */

    ngx_http_dynamic_update_upstream_create_srv_conf,  /* create server configuration */
    NULL,                                              /* merge server configuration */

    NULL,                                             /* create location configuration */
    NULL                                              /* merge main configuration */
};


ngx_module_t  ngx_http_dynamic_update_upstream_module = {
    NGX_MODULE_V1,
    &ngx_http_dynamic_update_upstream_module_ctx, /* module context */
    ngx_http_dynamic_update_upstream_commands,    /* module directives */
    NGX_HTTP_MODULE,                              /* module type */
    NULL,                                         /* init master */
    ngx_http_dynamic_update_upstream_init_module, /* init module */
    ngx_http_dynamic_update_upstream_init_process,/* init process */
    NULL,                                         /* init thread */
    NULL,                                         /* exit thread */
    NULL,                                         /* exit process */
    NULL,                                         /* exit master */
    NGX_MODULE_V1_PADDING
};


static char *
ngx_http_dynamic_update_upstream_consul_server(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char                                       *p = NULL;
    time_t                                        update_timeout = 0, update_interval = 0;
    time_t                                        delay_delete = 0;
    ngx_str_t                                    *value, s;
    ngx_url_t                                     u;
    ngx_uint_t                                    i, strong_dependency = 0;
    ngx_http_upstream_server_t                   *us;
    ngx_http_dynamic_update_upstream_srv_conf_t  *duscf;

    value = cf->args->elts;

    duscf = ngx_http_conf_get_module_srv_conf(cf,
                                              ngx_http_dynamic_update_upstream_module);
    us = &duscf->us;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "update_timeout=", 15) == 0) {

            s.len = value[i].len - 15;
            s.data = &value[i].data[15];

            update_timeout = ngx_parse_time(&s, 0);

            if (update_timeout == (time_t) NGX_ERROR) {
                goto invalid;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "update_interval=", 16) == 0) {

            s.len = value[i].len - 16;
            s.data = &value[i].data[16];

            update_interval = ngx_parse_time(&s, 0);

            if (update_interval == (time_t) NGX_ERROR) {
                goto invalid;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "delay_delete=", 13) == 0) {

            s.len = value[i].len - 13;
            s.data = &value[i].data[13];

            delay_delete = ngx_parse_time(&s, 0);

            if (delay_delete == (time_t) NGX_ERROR) {
                goto invalid;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "strong_dependency=", 18) == 0) {
            s.len = value[i].len - 18;
            s.data = value[i].data + 18;

            if (ngx_strcasecmp(s.data, (u_char *) "on") == 0) {
                strong_dependency = 1;
            } else if (ngx_strcasecmp(s.data, (u_char *) "off") == 0) {
                strong_dependency = 0;
            } else {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid value \"%s\", "
                                   "it must be \"on\" or \"off\"",
                                   value[i].data);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        goto invalid;
    }

    if (update_interval != 0) {
        duscf->update_interval = update_interval;
    }

    if (update_timeout != 0) {
        duscf->update_timeout = update_timeout;
    }

    if (delay_delete != 0) {
        duscf->delay_delete = delay_delete;
    }

    if (strong_dependency != 0) {
        duscf->strong_dependency = strong_dependency;
    }

    ngx_memzero(&u, sizeof(ngx_url_t));

    p = (u_char *)ngx_strchr(value[1].data, '/');
    if (p == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "dynamic_update_upstream_consul_server: please input consul upstream key in upstream");
        return NGX_CONF_ERROR;
    }
    duscf->update_send.data = p;
    duscf->update_send.len = value[1].len - (p - value[1].data);

    u.url.data = value[1].data;
    u.url.len = p - value[1].data;

    p = (u_char *)ngx_strchr(value[1].data, ':');
    if (p != NULL) {
        duscf->consul_host.data = value[1].data;
        duscf->consul_host.len = p - value[1].data;

        duscf->consul_port = ngx_atoi(p + 1, duscf->update_send.data - p - 1);
        if (duscf->consul_port < 1 || duscf->consul_port > 65535) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "dynamic_update_upstream_consul_server: consul server port is invalid");
            return NGX_CONF_ERROR;
        }

    } else {
        duscf->consul_host.data = value[1].data;
        duscf->consul_host.len = u.url.len;

        duscf->consul_port = 80;
    }

    u.default_port = 80;

    if (ngx_parse_url(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "dynamic_update_upstream_consul_server: %s in upstream \"%V\"", u.err, &u.url);
        }

        return NGX_CONF_ERROR;
    }

    us->name = u.url;
    us->addrs = u.addrs;
    us->naddrs = u.naddrs;
    us->weight = 1;
    us->max_fails = 1;
    us->fail_timeout = 10;

    return NGX_CONF_OK;

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                "dynamic_update_upstream_consul_server: consul invalid parameter \"%V\"", &value[i]);

    return NGX_CONF_ERROR;
}


static char *
ngx_http_dynamic_update_upstream_set_conf_dump(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                                        *value;
    ngx_http_dynamic_update_upstream_srv_conf_t      *duscf;

    duscf = ngx_http_conf_get_module_srv_conf(cf,
                                              ngx_http_dynamic_update_upstream_module);

    value = cf->args->elts;

    duscf->upstream_conf_path = value[1]; 
    if (duscf->upstream_conf_path.len == NGX_CONF_UNSET_SIZE) {
        return NGX_CONF_ERROR; 
    }

    duscf->conf_file = ngx_conf_open_file(cf->cycle, &value[1]); 
    if (duscf->conf_file == NULL) {
        return NGX_CONF_ERROR; 
    }

    return NGX_CONF_OK;
}


static void
ngx_http_dynamic_update_upstream_process(ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    char                                         *p;
    ngx_buf_t                                    *buf;
    ngx_int_t                                     index = 0;
    ngx_uint_t                                    i;
    ngx_uint_t                                    add_flag=0, del_flag=0, weight_flag=0;
    ngx_http_dynamic_update_upstream_ctx_t       *ctx;
    ngx_http_dynamic_update_upstream_srv_conf_t  *conf;

    ctx = &conf_server->ctx;
    conf = conf_server->conf;

    buf = &ctx->body;

    for (i = 0; i < state.num_headers; i++) {
        if (ngx_memcmp(state.headers[i][0], NGX_INDEX_HEARDER, 
                    NGX_INDEX_HEARDER_LEN) == 0) {

            p = ngx_strchr(state.headers[i][1], '\r');
            *p = '\0';
            index = ngx_atoi((u_char *)state.headers[i][1], 
                             (size_t)ngx_strlen((u_char *)state.headers[i][1]));
            break;
        }
    }

    if (index == conf_server->index) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "[NOTICE]:"
                "dynamic_update_upstream_process: index is not change in upstream: %V",
                &conf->upstream_conf_path);
        return;

    } else {
        conf_server->index = index;
    }

    if (ngx_http_dynamic_update_upstream_parse_json(buf->pos, conf_server) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "dynamic_update_upstream_process: parse json error");
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG, ngx_cycle->log, 0,
            "dynamic_update_upstream_process: parse json succeed");

    ngx_http_dynamic_update_upstream_add_check((ngx_cycle_t *)ngx_cycle, conf_server);
    if (ctx->add_upstream.nelts > 0) {

        if (ngx_http_dynamic_update_upstream_add_server((ngx_cycle_t *)ngx_cycle, 
                    conf_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                    "dynamic_update_upstream_process: upstream add server error");
            return;
        }
        add_flag = 1;
    }

    ngx_http_dynamic_update_upstream_del_check((ngx_cycle_t *)ngx_cycle, conf_server);
    if (ctx->del_upstream.nelts > 0) {

        if (ngx_http_dynamic_update_upstream_del_server((ngx_cycle_t *)ngx_cycle, 
                    conf_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                    "dynamic_update_upstream_process: upstream del server error");
            return;
        }
        del_flag = 1;
    }

    if (!add_flag && !del_flag) {
        if (ngx_http_dynamic_update_upstream_server_weight((ngx_cycle_t *)ngx_cycle, 
                    conf_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                    "dynamic_update_upstream_process: upstream change peer weight error");
            return;
        }
        weight_flag = 1;
    }

    if (add_flag || del_flag || weight_flag) {
        if (ngx_shmtx_trylock(&conf_server->dynamic_accept_mutex)) {

            ngx_http_dynamic_update_upstream_dump_conf(conf_server);
            ngx_shmtx_unlock(&conf_server->dynamic_accept_mutex);
        }
    }

    return;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_add_server(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    u_char                                  *port, *p, *last, *pp;
    ngx_int_t                                n;
    ngx_uint_t                               i;
    ngx_addr_t                              *addrs;
    ngx_array_t                              servers;  /* ngx_http_upstream_server_t */
    ngx_http_update_conf_t                  *conf;
    ngx_http_upstream_server_t              *server;
    ngx_http_dynamic_update_upstream_ctx_t  *ctx;

    struct sockaddr_in  *sin;

    ctx = &conf_server->ctx;

    if (ngx_array_init(&servers, ctx->pool, 16, sizeof(*server)) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "dynamic_update_upstream_add_server: alloc error");

        return NGX_ERROR;
    }

    for (i = 0; i < ctx->add_upstream.nelts; i++) {
        conf = (ngx_http_update_conf_t *)ctx->add_upstream.elts + i;

        p = conf->sockaddr;
        last = p + ngx_strlen(p);

        port = ngx_strlchr(p, last, ':');
        if (port == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "dynamic_update_upstream_add_server: has no port in %s", p);
            continue;
        }

        n = ngx_atoi(port + 1, last - port - 1);
        if (n < 1 || n > 65535) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "dynamic_update_upstream_add_server: invalid port in %s", p);
            continue;
        }

        sin = ngx_calloc(sizeof(struct sockaddr_in), cycle->log);
        if (sin == NULL) {
            goto invalid;
        }

        sin->sin_family = AF_INET;
        sin->sin_port = htons((in_port_t) n);
        sin->sin_addr.s_addr = ngx_inet_addr(p, port - p);

        if (sin->sin_addr.s_addr == INADDR_NONE) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "dynamic_update_upstream_add_server: invalid ip in %s", p);
            ngx_free(sin);
            continue;
        }

        addrs = ngx_calloc(sizeof(ngx_addr_t), cycle->log);
        if (addrs == NULL) {
            return NGX_ERROR;
        }

        addrs->sockaddr = (struct sockaddr *) sin;
        addrs->socklen = sizeof(struct sockaddr_in);

        pp = ngx_calloc(last - p, cycle->log);
        if (pp == NULL) {
            goto invalid;
        }

        addrs->name.len = ngx_sprintf(pp, "%s", p) - pp;
        addrs->name.data = pp;

        server = ngx_array_push(&servers);
        ngx_memzero(server, sizeof(ngx_http_upstream_server_t));

        server->addrs = addrs;
        server->naddrs = 1;

        server->down = conf->down;
        server->backup = conf->backup;
        server->weight = conf->weight;
        server->max_fails = conf->max_fails;
        server->fail_timeout = conf->fail_timeout;
    }

    if (servers.nelts > 0) {
        if (ngx_http_dynamic_update_upstream_add_peer(cycle, &servers, 
                    conf_server) != NGX_OK) {
            goto invalid;
        }
    }

    return NGX_OK;

invalid:

    for (i = 0; i < servers.nelts; i++) {
        server = (ngx_http_upstream_server_t *)servers.elts + i;

        if (server->addrs->sockaddr != NULL) {
            ngx_free(server->addrs->sockaddr);
        }

        if (server->addrs->name.data != NULL) {
            ngx_free(server->addrs->name.data);
        }
        
        ngx_free(server->addrs);
    }

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_add_peer(ngx_cycle_t *cycle,
        ngx_array_t *servers, ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    ngx_uint_t                     i=0, n=0, w=0, m=0;
    ngx_http_upstream_server_t    *server=NULL;
    ngx_http_upstream_rr_peers_t  *peers=NULL, *tmp_peers=NULL;
    ngx_http_upstream_srv_conf_t  *uscf;

    uscf = conf_server->uscf;

    if (servers->nelts < 1) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "dynamic_update_upstream_add_peer: no servers to add \"%V\"", &uscf->host);
        return NGX_ERROR;
    }

    if (uscf->peer.data != NULL) {
        tmp_peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
    }

    if (tmp_peers && servers->nelts >= 1) {
        n = tmp_peers->number + servers->nelts;

        peers = ngx_calloc(sizeof(ngx_http_upstream_rr_peers_t)
                + sizeof(ngx_http_upstream_rr_peer_t) * (n - 1), cycle->log);
        if (peers == NULL) {
            goto invalid;
        }
        ngx_memcpy(peers, tmp_peers, sizeof(ngx_http_upstream_rr_peers_t) +
                   + sizeof(ngx_http_upstream_rr_peer_t) * (tmp_peers->number));

        m = tmp_peers->number;
        for (i = 0; i < servers->nelts; i++) {

            server = (ngx_http_upstream_server_t *)servers->elts + i;
            if (server->backup) {
                continue;
            }

            peers->peer[m].sockaddr = server->addrs->sockaddr;
            peers->peer[m].socklen = server->addrs->socklen;
            peers->peer[m].name = server->addrs->name;
            peers->peer[m].max_fails = server->max_fails;
            peers->peer[m].fail_timeout = server->fail_timeout;
            peers->peer[m].down = server->down;
            peers->peer[m].weight = server->weight;
            peers->peer[m].effective_weight = server->weight;
            peers->peer[m].current_weight = 0;

#if (NGX_HTTP_UPSTREAM_CHECK) 
            ngx_uint_t index = ngx_http_upstream_check_add_dynamic_peer(cycle->pool, uscf, server->addrs);
            peers->peer[m].check_index = index;
#endif

            w += server->weight;
            m++;
        }
        w += tmp_peers->total_weight;

        peers->single = (n == 1);
        peers->number = n;
        peers->weighted = (w != n);
        peers->total_weight = w;

        uscf->peer.data = peers;
        peers->next = tmp_peers->next;

        ngx_http_dynamic_update_upstream_event_init(tmp_peers, conf_server, NGX_ADD);
    }

    return NGX_OK;

invalid:
    ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
            "dynamic_update_upstream_add_peer: add failed \"%V\"", &uscf->host);

    if (peers != NULL) {
        ngx_http_dynamic_update_upstream_free(peers);
    }
    peers = NULL;

    uscf->peer.data = tmp_peers;

    return NGX_ERROR;
}


static void
ngx_http_dynamic_update_upstream_add_check(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    ngx_uint_t                               i, j, len;
    ngx_http_update_conf_t                  *upstream_conf, *add_upstream;
    ngx_http_upstream_rr_peers_t            *peers=NULL;
    ngx_http_upstream_srv_conf_t            *uscf;
    ngx_http_dynamic_update_upstream_ctx_t  *ctx;

    ctx = &conf_server->ctx;

    if (ngx_array_init(&ctx->add_upstream, ctx->pool, 16,
                sizeof(*add_upstream)) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "dynamic_update_upstream_add_check: alloc error");

        return;
    }

    uscf = conf_server->uscf;

    if (uscf->peer.data != NULL) {
        peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    } else {
        return;
    }

    len = ctx->upstream_conf.nelts;

    for (i = 0; i < len; i++) {
        upstream_conf = (ngx_http_update_conf_t *)ctx->upstream_conf.elts + i;

        for (j = 0; j < peers->number; j++) {

            if (ngx_memcmp(peers->peer[j].name.data, 
                        upstream_conf->sockaddr, peers->peer[j].name.len) == 0) {

                break;
            }

        }

        if (j == peers->number) {

            add_upstream = ngx_array_push(&ctx->add_upstream);
            ngx_memcpy(add_upstream, upstream_conf, sizeof(*upstream_conf));
        }
    }

    return;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_del_server(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    u_char                                  *port, *p, *last, *pp;
    ngx_int_t                                n, j;
    ngx_uint_t                               i;
    ngx_pool_t                              *pool;
    ngx_addr_t                              *addrs;
    ngx_http_update_conf_t                  *conf;
    ngx_http_upstream_server_t               us;
    ngx_http_dynamic_update_upstream_ctx_t  *ctx;

    struct sockaddr_in  *sin;

    ctx = &conf_server->ctx;

    ngx_memzero(&us, sizeof(ngx_http_upstream_server_t));

    pool = ngx_create_pool(ngx_pagesize, cycle->log);
    if (pool == NULL) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                "dynamic_update_upstream_del_server: no enough memory");
        return NGX_ERROR;
    }

    addrs = ngx_pcalloc(pool, ctx->del_upstream.nelts * sizeof(ngx_addr_t));
    if (addrs == NULL) {
        goto invalid;
    }

    for (i = 0, j = 0; i < ctx->del_upstream.nelts; i++, j++) {
        conf = (ngx_http_update_conf_t *)ctx->del_upstream.elts + i;

        p = conf->sockaddr;
        last = p + ngx_strlen(p);

        port = ngx_strlchr(p, last, ':');
        if(port == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "dynamic_update_upstream_del_server: has no port in %s", p);
            j--;
            continue;
        }

        n = ngx_atoi(port + 1, last - port - 1);
        if (n < 1 || n > 65535) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "dynamic_update_upstream_del_server: invalid port in %s", p);
            j--;
            continue;
        }

        sin = ngx_pcalloc(pool, sizeof(struct sockaddr_in));
        if (sin == NULL) {
            goto invalid;
        }

        sin->sin_family = AF_INET;
        sin->sin_port = htons((in_port_t) n);
        sin->sin_addr.s_addr = ngx_inet_addr(p, port - p);

        if (sin->sin_addr.s_addr == INADDR_NONE) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "dynamic_update_upstream_del_server: invalid ip in %s", p);
            j--;
            continue;
        }

        addrs[j].sockaddr = (struct sockaddr *) sin;
        addrs[j].socklen = sizeof(struct sockaddr_in);

        pp = ngx_pcalloc(pool, last - p);
        if (pp == NULL) {
            goto invalid;
        }

        addrs[j].name.len = ngx_sprintf(pp, "%s", p) - pp;
        addrs[j].name.data = pp;
    }

    us.addrs = addrs;
    us.naddrs = j;
    us.weight = 1;
    us.max_fails = 0;
    us.fail_timeout = 0;

    if (us.naddrs > 0) {
        if (ngx_http_dynamic_update_upstream_del_peer(cycle, &us, 
                    conf_server) != NGX_OK) {
            goto invalid;
        }
    }

    ngx_destroy_pool(pool);

    return NGX_OK;

invalid:
    ngx_destroy_pool(pool);

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_del_peer(ngx_cycle_t *cycle,
        ngx_http_upstream_server_t *us, ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    ngx_uint_t                          i, j, n=0, w=0, len=0;
    ngx_http_upstream_rr_peers_t       *peers=NULL, *tmp_peers=NULL;
    ngx_http_upstream_srv_conf_t       *uscf;

    len = sizeof(struct sockaddr);

    uscf = conf_server->uscf;
    if (uscf->peer.data != NULL) {
        tmp_peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
    }

    if (tmp_peers->number <= us->naddrs) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "[WARN]:"
                "dynamic_update_upstream_del_peer: upstream \"%V\" cannt delete all", &uscf->host);

        return NGX_OK;
    }

    if (tmp_peers) {
        n = tmp_peers->number - us->naddrs;
        w = tmp_peers->total_weight;

        peers = ngx_calloc(sizeof(ngx_http_upstream_rr_peers_t)
                + sizeof(ngx_http_upstream_rr_peer_t) * (n - 1), cycle->log);
        if (peers == NULL) {
            goto invalid;
        }

        n = 0;
        for (i = 0; i < tmp_peers->number; i++) {
            for (j = 0; j < us->naddrs; j++) {
                if (ngx_memn2cmp((u_char *) tmp_peers->peer[i].sockaddr, 
                            (u_char *) us->addrs[j].sockaddr, len, len) == 0) {

#if (NGX_HTTP_UPSTREAM_CHECK) 
                    ngx_http_upstream_check_delete_dynamic_peer(
                                                      tmp_peers->name, &us->addrs[j]);
                    tmp_peers->peer[i].check_index = NGX_MAX_VALUE;
#endif

                    w -= tmp_peers->peer[i].weight;
                    break;
                }
            }

            if (j == us->naddrs) {

                peers->peer[n].sockaddr = tmp_peers->peer[i].sockaddr;
                peers->peer[n].socklen = tmp_peers->peer[i].socklen;
                peers->peer[n].name.len = tmp_peers->peer[i].name.len;
                peers->peer[n].name.data = tmp_peers->peer[i].name.data;
                peers->peer[n].max_fails = tmp_peers->peer[i].max_fails;
                peers->peer[n].fail_timeout = tmp_peers->peer[i].fail_timeout;
                peers->peer[n].down = tmp_peers->peer[i].down;
                peers->peer[n].weight = tmp_peers->peer[i].weight;
                peers->peer[n].effective_weight = tmp_peers->peer[i].effective_weight;
                peers->peer[n].current_weight = tmp_peers->peer[i].current_weight;

#if (NGX_HTTP_UPSTREAM_CHECK) 
                peers->peer[n].check_index = tmp_peers->peer[i].check_index;
#endif
                n++;
            }
        }

        peers->single = (n == 1);
        peers->number = n;
        peers->weighted = (w != n);
        peers->total_weight = w;
        peers->name = tmp_peers->name; //upstream host

        uscf->peer.data = peers;
        peers->next = tmp_peers->next;

        ngx_http_dynamic_update_upstream_event_init(tmp_peers, conf_server, NGX_DEL);
    }

    return NGX_OK;

invalid:
    ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
            "dynamic_update_upstream_del_peer: del failed \"%V\"", &uscf->host);

    if (peers != NULL) {
        ngx_http_dynamic_update_upstream_free(peers);
    }
    peers = NULL;

    uscf->peer.data = tmp_peers;

    return NGX_ERROR;
}


static void
ngx_http_dynamic_update_upstream_del_check(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    ngx_uint_t                               i, j, len;
    ngx_http_update_conf_t                  *upstream_conf, *del_upstream;
    ngx_http_upstream_rr_peers_t            *peers = NULL;
    ngx_http_upstream_srv_conf_t            *uscf;
    ngx_http_dynamic_update_upstream_ctx_t  *ctx;

    ctx = &conf_server->ctx;

    if (ngx_array_init(&ctx->del_upstream, ctx->pool, 16,
                sizeof(*del_upstream)) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "dynamic_update_upstream_del_check: alloc error");

        return;
    }

    uscf = conf_server->uscf;

    if (uscf->peer.data != NULL) {
        peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    } else {
        return;
    }

    len = ctx->upstream_conf.nelts;

    for (i = 0; i < peers->number; i++) {
        for (j = 0; j < len; j++) {

            upstream_conf = (ngx_http_update_conf_t *)ctx->upstream_conf.elts + j;
            if (ngx_memcmp(peers->peer[i].name.data, 
                        upstream_conf->sockaddr, peers->peer[i].name.len) == 0) {

                break;
            }
        }

        if (j == len) {

            del_upstream = ngx_array_push(&ctx->del_upstream);
            ngx_memzero(del_upstream, sizeof(*del_upstream));
            ngx_memcpy(&del_upstream->sockaddr, peers->peer[i].name.data, peers->peer[i].name.len);

        }
    }

    return;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_server_weight(ngx_cycle_t *cycle,
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    ngx_uint_t                               i, j, w=0, len=0;
    ngx_http_update_conf_t                  *upstream_conf;
    ngx_http_upstream_srv_conf_t            *uscf;
    ngx_http_upstream_rr_peers_t            *peers=NULL;
    ngx_http_dynamic_update_upstream_ctx_t  *ctx;

    ctx = &conf_server->ctx;

    uscf = conf_server->uscf;

    if (uscf->peer.data != NULL) {
        peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    } else {
        return NGX_ERROR;
    }

    if (peers->number != ctx->upstream_conf.nelts) {
        return NGX_ERROR;
    }

    len = ctx->upstream_conf.nelts;

    for (i = 0; i < peers->number; i++) {
        for (j = 0; j < len; j++) {

            upstream_conf = (ngx_http_update_conf_t *)ctx->upstream_conf.elts + j;
            if (ngx_memcmp(peers->peer[i].name.data, 
                        upstream_conf->sockaddr, peers->peer[i].name.len) == 0) {

                peers->peer[i].weight = upstream_conf->weight;
                w += upstream_conf->weight;

                break;
            }
        }
    }

    peers->weighted = (w != peers->number);
    peers->total_weight = w;

    return NGX_OK;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_parse_json(u_char *buf, 
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    u_char                                  *p;
    ngx_int_t                                max_fails=2, backup=0, down=0;
    ngx_str_t                                src, dst;
    ngx_http_update_conf_t                  *upstream_conf=NULL;
    ngx_http_dynamic_update_upstream_ctx_t  *ctx;

    ctx = &conf_server->ctx;
    ctx->upstream_count = 0;

    src.len = 0, src.data = NULL;
    dst.len = 0, dst.data = NULL;

    cJSON *root = cJSON_Parse((char *)buf);
    if (root == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "dynamic_update_upstream_parse_json: root error");

        return NGX_ERROR;
    }

    ctx->upstream_count++;
    if (ngx_array_init(&ctx->upstream_conf, ctx->pool, 16,
                sizeof(*upstream_conf)) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "dynamic_update_upstream_parse_json: array init error");

        return NGX_ERROR;
    }

    cJSON *server_next;
    for (server_next = root->child; server_next != NULL; 
            server_next = server_next->next) {

        cJSON *temp1 = cJSON_GetObjectItem(server_next, "Key");
        if (temp1 != NULL && temp1->valuestring != NULL) {
            p = (u_char *)ngx_strrchr(temp1->valuestring, '/');
            if (ngx_http_dynamic_update_upstream_check_key(p) != NGX_OK) {
                continue;
            }

            upstream_conf = ngx_array_push(&ctx->upstream_conf);
            ngx_memzero(upstream_conf, sizeof(*upstream_conf));
            ngx_sprintf(upstream_conf->sockaddr, "%*s", ngx_strlen(p + 1), p + 1);
        }
        temp1 = NULL;

        temp1 = cJSON_GetObjectItem(server_next, "Value");
        if (temp1 != NULL && temp1->valuestring != NULL) {

            src.data = (u_char *)temp1->valuestring;
            src.len = ngx_strlen(temp1->valuestring);

            if (dst.data == NULL) {
                dst.data = ngx_pcalloc(ctx->pool, 1024);
            } else {
                ngx_memzero(dst.data, 1024);
            }
            dst.len = 0;

            ngx_decode_base64(&dst, &src);
        }
        temp1 = NULL;

        /* default value, server attribute */
        upstream_conf->weight = 1;
        upstream_conf->max_fails = 2;
        upstream_conf->fail_timeout = 10;

        upstream_conf->down = 0;
        upstream_conf->backup = 0;

        p = NULL;

        if (dst.data != NULL && dst.len != 0) {

            p = dst.data;
            cJSON *sub_root = cJSON_Parse((char *)p);
            if (sub_root == NULL) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                        "dynamic_update_upstream_parse_json: parse server attribute json failed,"
                        "setting server attribute to default value");
                continue;
            }

            cJSON *sub_attribute = sub_root;
            cJSON *temp1 = cJSON_GetObjectItem(sub_attribute, "weight");
            if (temp1 != NULL) {

                if (temp1->valuestring != NULL) {
                    upstream_conf->weight = ngx_atoi((u_char *)temp1->valuestring, 
                                                (size_t)ngx_strlen(temp1->valuestring));

                } else if (temp1->valueint >= 0) {
                    upstream_conf->weight = temp1->valueint;
                }

            }
            temp1 = NULL;

            temp1 = cJSON_GetObjectItem(sub_attribute, "max_fails");
            if (temp1 != NULL) {

                if (temp1->valuestring != NULL) {
                    max_fails = ngx_atoi((u_char *)temp1->valuestring, 
                                                (size_t)ngx_strlen(temp1->valuestring));

                } else if (temp1->valueint >= 0) {
                    max_fails = temp1->valueint;
                }

            }
            temp1 = NULL;

            temp1 = cJSON_GetObjectItem(sub_attribute, "fail_timeout");
            if (temp1 != NULL){

                if (temp1->valuestring != NULL) {

                    upstream_conf->fail_timeout = ngx_atoi((u_char *)temp1->valuestring, 
                                                (size_t)ngx_strlen(temp1->valuestring));

                } else if (temp1->valueint >= 0) {
                    upstream_conf->fail_timeout = temp1->valueint;
                }

            }
            temp1 = NULL;

            temp1 = cJSON_GetObjectItem(sub_attribute, "down");
            if (temp1 != NULL) {
                    
                if (temp1->valueint != 0) {
                    down = temp1->valueint;
                } else if (temp1->valuestring != NULL) {

                    down = ngx_atoi((u_char *)temp1->valuestring, 
                                                (size_t)ngx_strlen(temp1->valuestring));
                }
            }
            temp1 = NULL;

            temp1 = cJSON_GetObjectItem(sub_attribute, "backup");
            if (temp1 != NULL) {
                    
                if (temp1->valueint != 0) {
                    backup = temp1->valueint;
                } else if (temp1->valuestring != NULL) {

                    backup = ngx_atoi((u_char *)temp1->valuestring, 
                                                (size_t)ngx_strlen(temp1->valuestring));
                }
            }
            temp1 = NULL;

            dst.len = 0;
            cJSON_Delete(sub_root);
        }

        if (upstream_conf->weight <= 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                        "dynamic_update_upstream_parse_json: \"weight\" value is invalid and seting to 1");
            upstream_conf->weight = 1;
        }

        if (max_fails < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                        "dynamic_update_upstream_parse_json: \"max_fails\" value is invalid, seting to 2");
        } else {
            upstream_conf->max_fails = (ngx_uint_t)max_fails;
        }

        if (upstream_conf->fail_timeout < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                        "dynamic_update_upstream_parse_json: \"fail_timeout\" value is invalid, seting to 10");
            upstream_conf->fail_timeout = 10;
        }

        if (down != 1 && down != 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                        "dynamic_update_upstream_parse_json: \"down\" value is invalid, seting to 0");
        } else {
            upstream_conf->down = (ngx_uint_t)down;
        }

        if (backup != 1 && backup != 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                        "dynamic_update_upstream_parse_json: \"backup\" value is invalid, seting to 0");
        } else {
            upstream_conf->backup = (ngx_uint_t)backup;
        }

    }
    cJSON_Delete(root);

    return NGX_OK;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_check_key(u_char *key)
{
    u_char          *last, *ip_p, *port_p;
    ngx_int_t        port;

    port_p = (u_char *)ngx_strchr(key, ':');
    if (port_p == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, 
                    "dynamic_update_upstream_check_key: has no port in %s", key);
        return NGX_ERROR;
    }

    ip_p = key + 1;
    if (ngx_inet_addr(ip_p, port_p - ip_p) == INADDR_NONE) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, 
                    "dynamic_update_upstream_check_key: invalid ip in %s", key);
        return NGX_ERROR;
    }

    last = ip_p + ngx_strlen(ip_p);
    port = ngx_atoi(port_p + 1, last - port_p - 1);
    if (port < 1 || port > 65535) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, 
                    "dynamic_update_upstream_check_key: invalid port in %s", key);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_http_dynamic_update_upstream_free(ngx_http_upstream_rr_peers_t *peers)
{
    if (peers != NULL) {
        ngx_free(peers);
        peers = NULL;
    }

    return;
}


static void *
ngx_http_dynamic_update_upstream_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_dynamic_update_upstream_main_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_dynamic_update_upstream_main_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->upstream_num = NGX_CONF_UNSET_UINT;

    conf->conf_server = NGX_CONF_UNSET_PTR;

    return conf;
}


static char *
ngx_http_dynamic_update_upstream_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_uint_t                                     i;
    ngx_http_upstream_srv_conf_t                 **uscfp;
    ngx_http_upstream_main_conf_t                 *umcf;
    ngx_http_dynamic_update_upstream_main_conf_t  *dumcf = conf;

    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);

    dumcf->conf_server = ngx_pcalloc(cf->pool, 
            umcf->upstreams.nelts * sizeof(ngx_http_dynamic_update_upstream_server_t));

    if (dumcf->conf_server == NULL) {
        return NGX_CONF_ERROR;
    }

    dumcf->upstream_num = 0;

    dynamic_upstream_ctx = dumcf;

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        if (ngx_http_dynamic_update_upstream_init_srv_conf(cf, uscfp[i], i) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


static void *
ngx_http_dynamic_update_upstream_create_srv_conf(ngx_conf_t *cf)
{
    ngx_http_dynamic_update_upstream_srv_conf_t  *duscf;

    duscf = ngx_pcalloc(cf->pool, sizeof(ngx_http_dynamic_update_upstream_srv_conf_t));
    if (duscf == NULL) {
        return NULL;
    }

    duscf->consul_host.len = NGX_CONF_UNSET_SIZE;
    duscf->consul_host.data = NGX_CONF_UNSET_PTR;

    duscf->consul_port = NGX_CONF_UNSET;

    duscf->upstream_conf_path.len = NGX_CONF_UNSET_SIZE;
    duscf->upstream_conf_path.data = NGX_CONF_UNSET_PTR;

    duscf->update_timeout = NGX_CONF_UNSET_MSEC;
    duscf->update_interval = NGX_CONF_UNSET_MSEC;

    duscf->delay_delete = NGX_CONF_UNSET_MSEC;

    duscf->strong_dependency = NGX_CONF_UNSET_UINT;

    duscf->conf_file = NGX_CONF_UNSET_PTR;

    ngx_memzero(&duscf->us, sizeof(duscf->us));

    return duscf;
}


static char *
ngx_http_dynamic_update_upstream_init_srv_conf(ngx_conf_t *cf, void *conf, ngx_uint_t num)
{
    u_char                                      *buf;
    ngx_http_upstream_srv_conf_t                *uscf = conf;
    ngx_http_dynamic_update_upstream_server_t   *conf_server;
    ngx_http_dynamic_update_upstream_srv_conf_t *duscf;

    if (uscf->srv_conf == NULL) {
        return NGX_CONF_OK;
    }

    duscf = ngx_http_conf_upstream_srv_conf(uscf, ngx_http_dynamic_update_upstream_module);

    if (duscf->consul_host.data == NGX_CONF_UNSET_PTR 
        && duscf->consul_host.len == NGX_CONF_UNSET_SIZE) {
        return NGX_CONF_OK;
    }

    dynamic_upstream_ctx->upstream_num++;

    conf_server = &dynamic_upstream_ctx->conf_server[dynamic_upstream_ctx->upstream_num - 1];

    if (conf_server == NULL) {
        return NGX_CONF_ERROR;
    }

    if (duscf->update_timeout == NGX_CONF_UNSET_MSEC) {
        duscf->update_timeout = 1000 * 60 * 6;
    }

    if (duscf->update_interval == NGX_CONF_UNSET_MSEC) {
        duscf->update_interval = 1000 * 5;
    }

    if (duscf->delay_delete == NGX_CONF_UNSET_MSEC) {
        duscf->delay_delete = 1000 * 75;
    }

    if (duscf->strong_dependency == NGX_CONF_UNSET_UINT) {
        duscf->strong_dependency = 0;
    }

    if (duscf->upstream_conf_path.len == NGX_CONF_UNSET_SIZE) {
        buf = ngx_pcalloc(cf->pool, ngx_strlen("/usr/local/nginx/conf/upstreams/upstream_.conf")
                          + uscf->host.len + 1);
        ngx_sprintf(buf, "/usr/local/nginx/conf/upstreams/upstream_%V.conf", &uscf->host);

        duscf->upstream_conf_path.data = buf;
        duscf->upstream_conf_path.len = ngx_strlen("/usr/local/nginx/conf/upstreams/upstream_.conf")
                                        + uscf->host.len;
    }

    duscf->conf_file = ngx_conf_open_file(cf->cycle, &duscf->upstream_conf_path); 
    if (duscf->conf_file == NULL) {
        return NGX_CONF_ERROR; 
    }

    conf_server->index = 0;

    conf_server->conf = duscf;
    conf_server->uscf = uscf;

    conf_server->host.len = uscf->host.len;
    conf_server->host.data = uscf->host.data;

    return NGX_CONF_OK;
}


static ngx_int_t 
ngx_http_dynamic_update_upstream_init_module(ngx_cycle_t *cycle)
{
    ngx_uint_t                                        i;
    ngx_http_dynamic_update_upstream_server_t        *conf_server;
    ngx_http_dynamic_update_upstream_srv_conf_t      *duscf;

    conf_server = dynamic_upstream_ctx->conf_server;

    if (ngx_http_dynamic_update_upstream_init_shm_mutex(cycle) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "dynamic_update_upstream_init_module:"
                " init shm mutex failed");

        return NGX_ERROR;
    }

    for (i = 0; i < dynamic_upstream_ctx->upstream_num; i++) {

        duscf = conf_server[i].conf;
        if (duscf->conf_file->fd != NGX_INVALID_FILE) {
            ngx_close_file(duscf->conf_file->fd);
            duscf->conf_file->fd = NGX_INVALID_FILE;
        }
        ngx_change_file_access(duscf->upstream_conf_path.data, 
                               S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH|S_IWOTH);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_init_shm_mutex(ngx_cycle_t *cycle)
{
    u_char                                           *shared, *file;
    size_t                                            size, cl;
    ngx_shm_t                                         shm;
    ngx_uint_t                                        i;
    ngx_http_dynamic_update_upstream_server_t        *conf_server;

    conf_server = dynamic_upstream_ctx->conf_server;

    if (*dynamic_shared_created) {
        shm.size = 128 * (*dynamic_shared_created);
        shm.log = cycle->log;
        shm.addr = (u_char *)(dynamic_shared_created);
        shm.name.len = sizeof("ngx_dynamic_update_upstream_shared_zone");
        shm.name.data = (u_char *)"ngx_dynamic_update_upstream_shared_zone";

        ngx_shm_free(&shm);
    }

    /* cl should be equal to or greater than cache line size */
    cl = 128;
    size = cl                                                 /*shared created flag*/
         + cl * dynamic_upstream_ctx->upstream_num;           /*dynamic_accept_mutex for every upstream*/

    shm.size = size;
    shm.log = cycle->log;
    shm.name.len = sizeof("ngx_dynamic_update_upstream_shared_zone");
    shm.name.data = (u_char *)"ngx_dynamic_update_upstream_shared_zone";

    if (ngx_shm_alloc(&shm) != NGX_OK) {
        return NGX_ERROR;
    }
    shared = shm.addr;

    dynamic_shared_created = (ngx_atomic_t *)shared;

    for (i = 0; i < dynamic_upstream_ctx->upstream_num; i++) {

#if (NGX_HAVE_ATOMIC_OPS)

        file = NULL;

#else

        file = ngx_pcalloc(cycle->pool, cycle->lock_file.len + ngx_strlen("dynamic") + 3);
        if (file == NULL) {
            return NGX_ERROR;
        }

        (void) ngx_sprintf(file, "%V%s%d%Z", &ngx_cycle->lock_file, "dynamic", i);

#endif

        if (ngx_shmtx_create(&conf_server[i].dynamic_accept_mutex, 
                    (ngx_shmtx_sh_t *)(shared + (i + 1) * cl), file) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    ngx_atomic_cmp_set(dynamic_shared_created, *dynamic_shared_created, 
            dynamic_upstream_ctx->upstream_num);

    return NGX_OK;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_init_process(ngx_cycle_t *cycle)
{
    char                                         *conf_value = NULL;
    ngx_int_t                                     status;
    ngx_uint_t                                    i, j;
    ngx_pool_t                                   *pool;
    ngx_http_dynamic_update_upstream_ctx_t       *ctx;
    ngx_http_dynamic_update_upstream_server_t    *conf_server;

    conf_server = dynamic_upstream_ctx->conf_server;

    for (i = 0; i < dynamic_upstream_ctx->upstream_num; i++) {

        ctx = &conf_server[i].ctx;
        ngx_memzero(ctx, sizeof(*ctx));

        ngx_http_dynamic_update_upstream_init_peers(cycle, &conf_server[i]);

        pool = ngx_create_pool(NGX_PAGE_COUNT * ngx_pagesize, ngx_cycle->log);
        if (pool == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "dynamic_update_upstream_init_process: recv no enough memory");
            return NGX_ERROR;
        }
        ctx->pool = pool;

        for (j = 0; j < NGX_HTTP_RETRY_TIMES; j++) {
            status = ngx_http_dynamic_update_upstream_get_all(cycle, 
                    &conf_server[i], &conf_value);
            if (status == NGX_OK) {
                break;
            }
        }

        if (status != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "dynamic_update_upstream_init_process: pull upstream conf failed");

            if (conf_server[i].conf->strong_dependency == 0) {
                ngx_destroy_pool(pool);
                ctx->pool = NULL;

                continue;
            }

            return NGX_ERROR;
        }

        ctx->recv.pos = (u_char *)conf_value;
        ctx->recv.last = (u_char *)(conf_value + ngx_strlen(conf_value));
        ctx->recv.end = (u_char *)(conf_value + ngx_pagesize);

        if (ngx_http_parser_init() == NGX_ERROR) {
            ngx_destroy_pool(pool);
            ngx_free(conf_value);
            return NGX_ERROR;
        }

        ngx_http_parser_execute(ctx);

        if (ctx->body.pos == ctx->body.last) {
            ngx_free(conf_value);
            conf_value = NULL;

            ngx_destroy_pool(pool);
            ctx->pool = NULL;

            continue;
        }

        ngx_http_dynamic_update_upstream_process(&conf_server[i]);

        ngx_free(conf_value);
        conf_value = NULL;

        ngx_destroy_pool(pool);
        ctx->pool = NULL;
    }

    ngx_http_dynamic_update_upstream_add_timers(cycle);

    return NGX_OK;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_init_peers(ngx_cycle_t *cycle,
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    ngx_uint_t                          i, n, len;
    ngx_http_upstream_rr_peers_t       *peers=NULL, *tmp_peers=NULL;
    ngx_http_upstream_srv_conf_t       *uscf;

    uscf = conf_server->uscf;

    u_char *namep = NULL;
    struct sockaddr *saddr = NULL;
    len = sizeof(struct sockaddr);

    ngx_queue_init(&conf_server->add_ev);
    ngx_queue_init(&conf_server->delete_ev);

    if (uscf->peer.data != NULL) {
        tmp_peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
    }

    if (tmp_peers) {
        n = tmp_peers->number;

        peers = ngx_calloc(sizeof(ngx_http_upstream_rr_peers_t)
                + sizeof(ngx_http_upstream_rr_peer_t) * (n - 1), cycle->log);

        if (peers == NULL) {
            goto invalid;
        }

        peers->single = tmp_peers->single;
        peers->number = tmp_peers->number;
        peers->weighted = tmp_peers->weighted;
        peers->total_weight = tmp_peers->total_weight;
        peers->name = tmp_peers->name;

        n = 0;

        for (i = 0; i < tmp_peers->number; i++) {

            if ((saddr = ngx_calloc(len, cycle->log)) == NULL) {
                goto invalid;
            }
            ngx_memcpy(saddr, tmp_peers->peer[i].sockaddr, len);
            peers->peer[i].sockaddr = saddr;

            peers->peer[i].socklen = tmp_peers->peer[i].socklen;
            peers->peer[i].name.len = tmp_peers->peer[i].name.len;

            if ((namep = ngx_calloc(tmp_peers->peer[i].name.len,
                                        cycle->log)) == NULL) {
                goto invalid;
            }
            ngx_memcpy(namep, tmp_peers->peer[i].name.data,
                        tmp_peers->peer[i].name.len);
            peers->peer[i].name.data = namep;

            peers->peer[i].max_fails = tmp_peers->peer[i].max_fails;
            peers->peer[i].fail_timeout = tmp_peers->peer[i].fail_timeout;
            peers->peer[i].down = tmp_peers->peer[i].down;
            peers->peer[i].weight = tmp_peers->peer[i].weight;
            peers->peer[i].effective_weight = tmp_peers->peer[i].effective_weight;
            peers->peer[i].current_weight = tmp_peers->peer[i].current_weight;

#if (NGX_HTTP_UPSTREAM_CHECK) 
            peers->peer[i].check_index = tmp_peers->peer[i].check_index;
#endif
        }

        uscf->peer.data = peers;
        peers->next = tmp_peers->next;

        ngx_pfree(cycle->pool, tmp_peers);
    }

    return NGX_OK;

invalid:
    ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
            "dynamic_update_upstream_init_peers: copy failed \"%V\"", &uscf->host);

    if (peers != NULL) {
        ngx_http_dynamic_update_upstream_free(peers);
    }
    uscf->peer.data = tmp_peers;

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_add_timers(ngx_cycle_t *cycle)
{
    ngx_msec_t                                   t, tmp;
    ngx_uint_t                                   i;
    ngx_http_dynamic_update_upstream_server_t   *peer;
    ngx_http_dynamic_update_upstream_srv_conf_t *conf;

    peer = dynamic_upstream_ctx->conf_server;
    if (peer == NULL) {
        return NGX_OK;
    }
    conf = peer->conf;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, cycle->log, 0,
            "dynamic_update_upstream_add_timers: shm_name: %V", peer->pc.name);

    srandom(ngx_pid);
    for (i = 0; i < dynamic_upstream_ctx->upstream_num; i++) {

        peer[i].update_ev.handler = ngx_http_dynamic_update_upstream_begin_handler;
        peer[i].update_ev.log = cycle->log;
        peer[i].update_ev.data = &peer[i];
        peer[i].update_ev.timer_set = 0;

        peer[i].update_timeout_ev.handler =
            ngx_http_dynamic_update_upstream_timeout_handler;
        peer[i].update_timeout_ev.log = cycle->log;
        peer[i].update_timeout_ev.data = &peer[i];
        peer[i].update_timeout_ev.timer_set = 0;

        peer[i].send_handler = ngx_http_dynamic_update_upstream_send_handler;
        peer[i].recv_handler = ngx_http_dynamic_update_upstream_recv_handler;

        /*
         * We add a random start time here, since we don't want to trigger
         * the check events too close to each other at the beginning.
         */
        tmp = conf->update_interval;
        t = ngx_random() % 1000 + tmp;

        ngx_add_timer(&peer[i].update_ev, t);
    }

    return NGX_OK;
}


static void
ngx_http_dynamic_update_upstream_begin_handler(ngx_event_t *event)
{
    ngx_http_dynamic_update_upstream_ctx_t     *ctx;
    ngx_http_dynamic_update_upstream_server_t  *peer;

    if (ngx_http_dynamic_update_upstream_need_exit()) {
        return;
    }

    peer = event->data;
    if (peer == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "ngx_http_dynamic_update_upstream_begin_handler: peer is null");
        return;
    }

    ctx = &peer->ctx;
    if (ctx->pool != NULL) {
        ngx_destroy_pool(ctx->pool);
    }
    ctx->pool = NULL;

    ngx_memzero(ctx, sizeof(*ctx));

    if (parser != NULL) {
        ngx_free(parser);
    }
    parser = NULL;

    if (peer->update_ev.timer_set) {
        ngx_del_timer(&peer->update_ev);
    }

    ngx_http_dynamic_update_upstream_connect_handler(event);
}


static void
ngx_http_dynamic_update_upstream_connect_handler(ngx_event_t *event)
{
    ngx_int_t                                    rc;
    ngx_connection_t                            *c;
    ngx_http_dynamic_update_upstream_server_t   *peer;
    ngx_http_dynamic_update_upstream_srv_conf_t *conf;

    if (ngx_http_dynamic_update_upstream_need_exit()) {
        return;
    }

    if (ngx_http_dynamic_update_upstream_init_consul(event) != NGX_OK) {
        return;
    }

    peer = event->data;
    conf = peer->conf;
    ngx_add_timer(&peer->update_timeout_ev, conf->update_timeout);

    rc = ngx_event_connect_peer(&peer->pc);

    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        ngx_log_error(NGX_LOG_ERR, event->log, 0,
                "dynamic_update_upstream_connect_handler: cant connect with peer: %V ",
                peer->pc.name);
        return;
    }

    /* NGX_OK or NGX_AGAIN */
    c = peer->pc.connection;
    c->data = peer;
    c->log = peer->pc.log;
    c->sendfile = 0;
    c->read->log = c->log;
    c->write->log = c->log;

    c->write->handler = peer->send_handler;
    c->read->handler = peer->recv_handler;

    /* The kqueue's loop interface needs it. */
    if (rc == NGX_OK) {
        c->write->handler(c->write);
    }
}


static void
ngx_http_dynamic_update_upstream_send_handler(ngx_event_t *event)
{
    ssize_t                                       size;
    ngx_connection_t                             *c;
    ngx_http_dynamic_update_upstream_ctx_t       *ctx;
    ngx_http_dynamic_update_upstream_server_t    *peer;
    ngx_http_dynamic_update_upstream_srv_conf_t  *conf;

    if (ngx_http_dynamic_update_upstream_need_exit()) {
        return;
    }

    c = event->data;
    peer = c->data;

    conf = peer->conf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "dynamic_update_upstream_send");

    ctx = &peer->ctx;

    u_char request[NGX_BLOCK];
    ngx_memzero(request, NGX_BLOCK);
    ngx_sprintf(request, "GET %V?recurse&index=%d HTTP/1.0\r\nHost: %V\r\nAccept: */*\r\n\r\n", 
            &conf->update_send, peer->index, &conf->us.name);

    ctx->send.pos = request;
    ctx->send.last = ctx->send.pos + ngx_strlen(request);

    while (ctx->send.pos < ctx->send.last) {

        size = c->send(c, ctx->send.pos, ctx->send.last - ctx->send.pos);

#if (NGX_DEBUG)
        {
            ngx_err_t  err;

            err = (size >=0) ? 0 : ngx_socket_errno;
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, err,
                    "dynamic_update_upstream_send: send size: %z, total: %z",
                    size, ctx->send.last - ctx->send.pos);
        }
#endif

        if (size > 0) {
            ctx->send.pos += size;

        } else if (size == 0 || size == NGX_AGAIN) {
            return;

        } else {
            c->error = 1;
            goto update_send_fail;
        }
    }

    if (ctx->send.pos == ctx->send.last) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, 
                "dynamic_update_upstream_send: send done.");
    }

    return;

update_send_fail:
    ngx_log_error(NGX_LOG_ERR, event->log, 0,
            "dynamic_update_upstream_send: send error with peer: %V", peer->pc.name);

    ngx_http_dynamic_update_upstream_clean_event(peer);
}


static void
ngx_http_dynamic_update_upstream_recv_handler(ngx_event_t *event)
{
    u_char                                    *new_buf;
    ssize_t                                    size, n;
    ngx_pool_t                                *pool;
    ngx_connection_t                          *c;
    ngx_http_dynamic_update_upstream_ctx_t    *ctx;
    ngx_http_dynamic_update_upstream_server_t *peer;

    if (ngx_http_dynamic_update_upstream_need_exit()) {
        return;
    }

    c = event->data;
    peer = c->data;

    ctx = &peer->ctx;

    if (ctx->pool == NULL) {
        pool = ngx_create_pool(NGX_PAGE_COUNT * ngx_pagesize, ngx_cycle->log);

        if (pool == NULL) {
            ngx_log_error(NGX_LOG_ERR, event->log, 0, 
                    "dynamic_update_upstream_recv: recv no enough memory");
            return;
        }

        ctx->pool = pool;
    } else {
        pool = ctx->pool;
    }

    if (ctx->recv.start == NULL) {
        /* 1 of the page_size, is it enough? */
        ctx->recv.start = ngx_pcalloc(pool, ngx_pagesize);
        if (ctx->recv.start == NULL) {
            goto update_recv_fail;
        }

        ctx->recv.last = ctx->recv.pos = ctx->recv.start;
        ctx->recv.end = ctx->recv.start + ngx_pagesize;
    }

    while (1) {
        n = ctx->recv.end - ctx->recv.last;

        /* buffer not big enough? enlarge it by twice */
        if (n == 0) {
            size = ctx->recv.end - ctx->recv.start;
            new_buf = ngx_pcalloc(pool, size * 2);
            if (new_buf == NULL) {
                goto update_recv_fail;
            }

            ngx_memcpy(new_buf, ctx->recv.start, size);

            ctx->recv.pos = ctx->recv.start = new_buf;
            ctx->recv.last = new_buf + size;
            ctx->recv.end = new_buf + size * 2;

            n = ctx->recv.end - ctx->recv.last;
        }

        size = c->recv(c, ctx->recv.last, n);

#if (NGX_DEBUG)
        {
            ngx_err_t  err;

            err = (size >= 0) ? 0 : ngx_socket_errno;
            ngx_log_debug2(NGX_LOG_DEBUG, c->log, err,
                    "dynamic_update_upstream_recv: recv size: %z, peer: %V ",
                    size, peer->pc.name);
        }
#endif

        if (size > 0) {
            ctx->recv.last += size;
            continue;
        } else if (size == 0) {
            break;
        } else if (size == NGX_AGAIN) {
            return;
        } else {
            c->error = 1;
            goto update_recv_fail;
        }
    }

    if (ctx->recv.last != ctx->recv.pos) {

        if (ngx_http_parser_init() == NGX_ERROR) {
            goto update_recv_fail;
        }

        ngx_http_parser_execute(ctx);

        if (ctx->body.pos != ctx->body.last) {
            *(ctx->body.last + 1) = '\0';

            ngx_http_dynamic_update_upstream_process(peer);
        }
    }

    ngx_http_dynamic_update_upstream_clean_event(peer);

    return;

update_recv_fail:
    ngx_log_error(NGX_LOG_ERR, event->log, 0,
            "dynamic_update_upstream_recv: recv error with peer: %V", peer->pc.name);

    ngx_http_dynamic_update_upstream_clean_event(peer);
}


static ngx_int_t
ngx_http_dynamic_update_upstream_dump_conf(ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    ngx_buf_t                                       *b=NULL;
    ngx_uint_t                                       i, page_numbers;
    ngx_http_upstream_rr_peers_t                    *peers=NULL;
    ngx_http_upstream_srv_conf_t                    *uscf=NULL;
    ngx_http_dynamic_update_upstream_srv_conf_t     *duscf=NULL;

#if (NGX_HTTP_UPSTREAM_CHECK)
    u_char                                          *escaped_send=NULL;
    ngx_http_upstream_check_srv_conf_t              *ucscf=NULL;
#endif

    uscf = conf_server->uscf;

    if (uscf->peer.data != NULL) {
        peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    } else {
        ngx_log_error(NGX_LOG_ERR, conf_server->ctx.pool->log, 0,
                "dynamic_update_upstream_dump_conf: no peers");

        return NGX_ERROR;
    }

    if (peers->number == 0) {
        ngx_log_error(NGX_LOG_ERR, conf_server->ctx.pool->log, 0,
                "dynamic_update_upstream_dump_conf: peers number is zero");

        return NGX_ERROR;
    }
    page_numbers = peers->number / NGX_BACKEND_NUMBER + 1;

    b = ngx_create_temp_buf(conf_server->ctx.pool, page_numbers * ngx_pagesize);
    if (b == NULL) {
        ngx_log_error(NGX_LOG_ERR, conf_server->ctx.pool->log, 0,
                "dynamic_update_upstream_dump_conf: dump failed %V", &uscf->host);

        return NGX_ERROR;
    }

    b->last = ngx_snprintf(b->last,b->end - b->last, "upstream %V {\n", &uscf->host);
    b->last = ngx_snprintf(b->last,b->end - b->last, "\tkeepalive %d;\n", peers->number);
    b->last = ngx_snprintf(b->last,b->end - b->last, "\n\tconsul %V:%d%V update_interval=%dms update_timeout=%dms;\n", 
                                                                   &conf_server->conf->consul_host,
                                                                    conf_server->conf->consul_port,
                                                                   &conf_server->conf->update_send,
                                                                    conf_server->conf->update_interval,
                                                                    conf_server->conf->update_timeout);
    b->last = ngx_snprintf(b->last,b->end - b->last, "\tupstream_conf_path %V;\n\n", 
                                                                   &conf_server->conf->upstream_conf_path);

    for (i = 0; i < peers->number; i++) {
        b->last = ngx_snprintf(b->last,b->end - b->last, "\tserver %V", &peers->peer[i].name);
        b->last = ngx_snprintf(b->last,b->end - b->last, " weight=%d", peers->peer[i].weight);
        b->last = ngx_snprintf(b->last,b->end - b->last, " max_fails=%d", peers->peer[i].max_fails);
        b->last = ngx_snprintf(b->last,b->end - b->last, " fail_timeout=%ds;\n", peers->peer[i].fail_timeout);
    }

#if (NGX_HTTP_UPSTREAM_CHECK)
    ucscf = ngx_http_conf_upstream_srv_conf(uscf, ngx_http_upstream_check_module);
    if (ucscf != NULL && ucscf->check_type_conf != NULL) {
        escaped_send = ngx_pcalloc(conf_server->ctx.pool, ucscf->send.len * 2);
        ngx_print_escape(escaped_send, ucscf->send.data, ucscf->send.len);

        b->last = ngx_snprintf(b->last,b->end - b->last, "\n\tcheck interval=%d rise=%d fall=%d timeout=%d type=%V"
                                                     " default_down=%s;\n", ucscf->check_interval, ucscf->rise_count,
                                                     ucscf->fall_count, ucscf->check_timeout, &ucscf->check_type_conf->name,
                                                     ucscf->default_down ? "true":"false");
        b->last = ngx_snprintf(b->last,b->end - b->last, "\tcheck_keepalive_requests %d;\n", ucscf->check_keepalive_requests);
        b->last = ngx_snprintf(b->last,b->end - b->last, "\tcheck_http_send \"%s\";\n", escaped_send);
        b->last = ngx_snprintf(b->last,b->end - b->last, "\tcheck_http_expect_alive http_2xx http_3xx;\n");
    }
#endif

    b->last = ngx_snprintf(b->last,b->end - b->last, "}\n");

    duscf = conf_server->conf;
    duscf->conf_file->fd = ngx_open_file(duscf->upstream_conf_path.data,
                                         NGX_FILE_TRUNCATE,
                                         NGX_FILE_WRONLY,
                                         NGX_FILE_DEFAULT_ACCESS);
    if (duscf->conf_file->fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, conf_server->ctx.pool->log, 0,
                        "dynamic_update_upstream_dump_conf: open dump file \"%V\" failed", 
                        &duscf->upstream_conf_path);
        return NGX_ERROR;
    }

    ngx_lseek(duscf->conf_file->fd, 0, SEEK_SET);
    if (ngx_write_fd(duscf->conf_file->fd, b->start, b->last - b->start) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, conf_server->ctx.pool->log, 0,
                "dynamic_update_upstream_dump_conf: write file failed %V", 
                &duscf->upstream_conf_path);
        ngx_close_file(duscf->conf_file->fd);

        return NGX_ERROR;
    }

    if (ngx_ftruncate(duscf->conf_file->fd, b->last - b->start) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, conf_server->ctx.pool->log, 0,
                "dynamic_update_upstream_dump_conf: truncate file failed %V", 
                &duscf->upstream_conf_path);
        ngx_close_file(duscf->conf_file->fd);

        return NGX_ERROR;
    }
    ngx_close_file(duscf->conf_file->fd);
    duscf->conf_file->fd = NGX_INVALID_FILE;

    ngx_log_error(NGX_LOG_ERR, conf_server->ctx.pool->log, 0, "[NOTICE]:"
                  "dynamic_update_upstream_dump_conf: dump conf file %V succeed, server numbers is %d", 
                  &duscf->upstream_conf_path, peers->number);

    return NGX_OK;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_init_consul(ngx_event_t *event)
{
    ngx_pool_t                                   *pool;
    ngx_http_upstream_server_t                   *us;
    ngx_http_dynamic_update_upstream_ctx_t       *ctx;
    ngx_http_dynamic_update_upstream_server_t    *peer;
    ngx_http_dynamic_update_upstream_srv_conf_t  *conf;

    u_char               *p, *host = NULL;
    size_t                len;
    ngx_str_t            *name;
    struct addrinfo       hints, *res=NULL, *rp=NULL;
    struct sockaddr_in   *sin;

    peer = event->data;
    conf = peer->conf;
    us = &conf->us;

    ctx = &peer->ctx;
    if (ctx->pool == NULL) {

        pool = ngx_create_pool(NGX_PAGE_COUNT * ngx_pagesize, ngx_cycle->log);
        if (pool == NULL) {
            ngx_log_error(NGX_LOG_ERR, event->log, 0, 
                        "dynamic_update_upstream_init_consul: creat pool, no enough memory");
            return NGX_ERROR;
        }
        ctx->pool = pool;
    }

    ngx_memzero(&peer->pc, sizeof(ngx_peer_connection_t));

    peer->pc.get = ngx_event_get_peer;
    peer->pc.log = event->log;
    peer->pc.log_error = NGX_ERROR_ERR;

    peer->pc.cached = 0;
    peer->pc.connection = NULL;

    if (ngx_inet_addr(conf->consul_host.data, conf->consul_host.len)
            == INADDR_NONE) {

        host = ngx_pcalloc(ctx->pool, conf->consul_host.len + 1);
        if (host == NULL) {
            return NGX_ERROR;
        }

        (void) ngx_cpystrn(host, conf->consul_host.data, conf->consul_host.len + 1);

        ngx_memzero(&hints, sizeof(struct addrinfo));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
#ifdef AI_ADDRCONFIG
        hints.ai_flags = AI_ADDRCONFIG;
#endif

        if (getaddrinfo((char *) host, NULL, &hints, &res) != 0) {
            res = NULL;
            goto valid;
        }

        for (rp = res; rp != NULL; rp = rp->ai_next) {

            if (rp->ai_family != AF_INET) {
                continue;
            }

            sin = ngx_pcalloc(ctx->pool, rp->ai_addrlen);
            if (sin == NULL) {
                goto valid;
            }

            ngx_memcpy(sin, rp->ai_addr, rp->ai_addrlen);

            sin->sin_port = htons((in_port_t) conf->consul_port);

            peer->pc.sockaddr = (struct sockaddr *) sin;
            peer->pc.socklen = rp->ai_addrlen;

            len = NGX_INET_ADDRSTRLEN + sizeof(":65535") - 1;

            p = ngx_pcalloc(ctx->pool, len);
            if (p == NULL) {
                goto valid;
            }

            len = ngx_sock_ntop((struct sockaddr *) sin, rp->ai_addrlen, p, len, 1);

            name = ngx_pcalloc(ctx->pool, sizeof(*name));
            if (name == NULL) {
                goto valid;
            }

            name->len = len;
            name->data = p;

            peer->pc.name = name;

            freeaddrinfo(res);
            return NGX_OK;
        }
    }

valid:
    peer->pc.sockaddr = us->addrs[0].sockaddr;
    peer->pc.socklen = us->addrs[0].socklen;
    peer->pc.name = &us->addrs[0].name;

    if (res != NULL) {
        freeaddrinfo(res);
    }

    return NGX_OK;
}


static void
ngx_http_dynamic_update_upstream_event_init(ngx_http_upstream_rr_peers_t *tmp_peers, 
        ngx_http_dynamic_update_upstream_server_t *conf_server, ngx_flag_t flag)
{
    ngx_time_t                                  *tp;
    ngx_delay_event_t                           *delay_event;
    ngx_http_dynamic_update_upstream_srv_conf_t *conf;

    conf = conf_server->conf;

    delay_event = ngx_calloc(sizeof(*delay_event), ngx_cycle->log);
    if (delay_event == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "dynamic_update_upstream_event_init: calloc failed");
        return;
    }

    tp = ngx_timeofday();
    delay_event->start_sec = tp->sec;
    delay_event->start_msec = tp->msec;

    if (flag == NGX_ADD) {
        delay_event->delay_delete_ev.handler = ngx_http_dynamic_update_upstream_add_delay_delete;
        delay_event->delay_delete_ev.log = ngx_cycle->log;
        delay_event->delay_delete_ev.data = delay_event;
        delay_event->delay_delete_ev.timer_set = 0;

        ngx_queue_insert_head(&conf_server->add_ev, &delay_event->queue);
    } else {

        delay_event->delay_delete_ev.handler = ngx_http_dynamic_update_upstream_del_delay_delete;
        delay_event->delay_delete_ev.log = ngx_cycle->log;
        delay_event->delay_delete_ev.data = delay_event;
        delay_event->delay_delete_ev.timer_set = 0;

        ngx_queue_insert_head(&conf_server->delete_ev, &delay_event->queue);
    }

    delay_event->data = tmp_peers;
    ngx_add_timer(&delay_event->delay_delete_ev, conf->delay_delete);

    return;
}


static void
ngx_http_dynamic_update_upstream_add_delay_delete(ngx_event_t *event)
{
    ngx_uint_t                     i;
    ngx_connection_t              *c;
    ngx_delay_event_t             *delay_event;
    ngx_http_request_t            *r=NULL;
    ngx_http_log_ctx_t            *ctx=NULL;
    ngx_http_upstream_rr_peers_t  *tmp_peers=NULL;

    delay_event = event->data;
    if (delay_event == NULL) {
        return;
    }
    tmp_peers = delay_event->data;

    c = ngx_cycle->connections;
    for (i = 0; i < ngx_cycle->connection_n; i++) {

        if (c[i].fd == (ngx_socket_t) -1) {
            continue;
        } else {

            if (c[i].log->data != NULL) {
                ctx = c[i].log->data;
                r = ctx->current_request;
            }
        }

        if (r) {
            if (r->start_sec < delay_event->start_sec) {
                ngx_add_timer(&delay_event->delay_delete_ev, NGX_DELAY_DELETE);
                return;
            }

            if (r->start_sec == delay_event->start_sec) {

                if (r->start_msec <= delay_event->start_msec) {
                    ngx_add_timer(&delay_event->delay_delete_ev, NGX_DELAY_DELETE);
                    return;
                }
            }
        }
    }

    if (tmp_peers != NULL) {

        ngx_free(tmp_peers);
        tmp_peers = NULL;
    }

    ngx_queue_remove(&delay_event->queue);
    ngx_free(delay_event);

    delay_event = NULL;

    return;
}


static void
ngx_http_dynamic_update_upstream_del_delay_delete(ngx_event_t *event)
{
    ngx_uint_t                     i;
    ngx_connection_t              *c;
    ngx_delay_event_t             *delay_event;
    ngx_http_request_t            *r=NULL;
    ngx_http_log_ctx_t            *ctx=NULL;
    ngx_http_upstream_rr_peers_t  *tmp_peers=NULL;

    u_char *namep = NULL;
    struct sockaddr *saddr = NULL;

    delay_event = event->data;
    if (delay_event == NULL) {
        return;
    }
    tmp_peers = delay_event->data;

    c = ngx_cycle->connections;
    for (i = 0; i < ngx_cycle->connection_n; i++) {

        if (c[i].fd == (ngx_socket_t) -1) {
            continue;
        } else {

            if (c[i].log->data != NULL) {
                ctx = c[i].log->data;
                r = ctx->request;
            }
        }

        if (r) {
            if (r->start_sec < delay_event->start_sec) {
                ngx_add_timer(&delay_event->delay_delete_ev, NGX_DELAY_DELETE);
                return;
            }

            if (r->start_sec == delay_event->start_sec) {

                if (r->start_msec <= delay_event->start_msec) {
                    ngx_add_timer(&delay_event->delay_delete_ev, NGX_DELAY_DELETE);
                    return;
                }
            }
        }
    }

    if (tmp_peers != NULL) {
        for (i = 0; i < tmp_peers->number; i++) {

#if (NGX_HTTP_UPSTREAM_CHECK) 
            if (tmp_peers->peer[i].check_index != NGX_MAX_VALUE) {
                continue;
            }
#endif

            saddr = tmp_peers->peer[i].sockaddr;
            if (saddr != NULL) {
                ngx_free(saddr);
            }

            namep = tmp_peers->peer[i].name.data;
            if (namep != NULL) {
                ngx_free(namep);
            }

            saddr = NULL, namep = NULL;
        }
    }

    if (tmp_peers != NULL) {
        ngx_free(tmp_peers);
        tmp_peers = NULL;
    }

    ngx_queue_remove(&delay_event->queue);
    ngx_free(delay_event);

    delay_event = NULL;

    return;
}


u_char *
ngx_print_escape(u_char *dst, u_char *src, size_t len)
{
    u_char    ch;

    if (dst == NULL || src == NULL || len < 1) {
        return NULL;
    }

    while (len) {
        ch = *src++;

        switch (ch) {
            case '\r':
                *dst++ = '\\';
                *dst++ = 'r';
                break;
            case '\n':
                *dst++ = '\\';
                *dst++ = 'n';
                break;
            default:
                *dst++ = ch;
        }
        
        len--;
    }

    *dst = '\0';

    return dst;
}


size_t
ngx_strnlen(const char *s, size_t maxlen)
{
  const char *p;

  p = ngx_strchr(s, '\0');
  if (p == NULL)
    return maxlen;

  return p - s;
}


size_t
ngx_strlncat(char *dst, size_t len, const char *src, size_t n)
{
  size_t slen;
  size_t dlen;
  size_t rlen;
  size_t ncpy;

  slen = ngx_strnlen(src, n);
  dlen = ngx_strnlen(dst, len);

  if (dlen < len) {
    rlen = len - dlen;
    ncpy = slen < rlen ? slen : (rlen - 1);
    ngx_memcpy(dst + dlen, src, ncpy);
    dst[dlen + ncpy] = '\0';
  }

  return slen + dlen;
}


static ngx_int_t
ngx_http_parser_init()
{
    ngx_memzero(state.status, 3);
    ngx_memzero(state.http_body, NGX_PAGESIZE * NGX_BLOCK);
    ngx_memzero(state.headers, NGX_MAX_HEADERS * 2 * NGX_MAX_ELEMENT_SIZE);

    state.num_headers = 0;
    state.last_header = NONE;

    parser = ngx_calloc(sizeof(http_parser), ngx_cycle->log);
    if (parser == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "ngx_http_parser_init: ngx_calloc is wrong");
        return NGX_ERROR;
    }

    http_parser_init(parser, HTTP_RESPONSE);

    return NGX_OK;
}


static void
ngx_http_parser_execute(ngx_http_dynamic_update_upstream_ctx_t *ctx)
{
    char      *buf;
    size_t     parsed;

    buf = (char *)ctx->recv.pos;

    ctx->body.pos = ctx->body.last = NULL;

    parsed = http_parser_execute(parser, &settings, buf, ngx_strlen(buf));
    if (parsed != ngx_strlen(buf)) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "http_parser_execute: parsed body size is wrong");
        return;
    }

    if (ngx_strncmp(state.status, "OK", 2) == 0) {

        if (ngx_strlen(state.http_body) != 0) {
            ctx->body.pos = state.http_body;
            ctx->body.last = state.http_body + ngx_strlen(state.http_body);

        } else if (ngx_strlen(state.http_body) == 0) {
            return;
        }
    }

    ngx_free(parser);
    parser = NULL;

    return;
}


int
ngx_http_status(http_parser *p, const char *buf, size_t len)
{
    if (p != parser) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "ngx_http_status: parser argument is wrong");

        return -1;
    }

    ngx_memcpy(state.status, buf, len);

    return 0;
}


int
ngx_http_header_field_cb (http_parser *p, const char *buf, size_t len)
{
    if (p != parser) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "ngx_http_header_field_cb: parser argument is wrong");

        return -1;
    }

    if (state.last_header != FIELD) {
        state.num_headers++;
    }

    ngx_strlncat(state.headers[state.num_headers-1][0],
                 sizeof(state.headers[state.num_headers-1][0]),
                 buf,
                 len);

    state.last_header = FIELD;

    return 0;
}


int
ngx_http_header_value_cb (http_parser *p, const char *buf, size_t len)
{
    if (p != parser) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "ngx_http_header_field_cb: parser argument is wrong");

        return -1;
    }

    ngx_strlncat(state.headers[state.num_headers-1][1],
                 sizeof(state.headers[state.num_headers-1][1]),
                 buf,
                 len);

    state.last_header = VALUE;

  return 0;
}


int
ngx_http_body(http_parser *p, const char *buf, size_t len)
{
    char *tmp_buf;

    tmp_buf = (char *)state.http_body;

    if (p != parser) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "ngx_http_body: parser argument is wrong");

        return -1;
    }

    ngx_memcpy(tmp_buf, buf, len);

    tmp_buf += len;

    return 0;
}


static void
ngx_http_dynamic_update_upstream_timeout_handler(ngx_event_t *event)
{
    ngx_http_dynamic_update_upstream_server_t  *peer;

    if (ngx_http_dynamic_update_upstream_need_exit()) {
        return;
    }

    peer = event->data;

    ngx_log_error(NGX_LOG_ERR, event->log, 0,
            "dynamic_update_upstream_timeout: time out with peer: %V ", peer->pc.name);

    ngx_http_dynamic_update_upstream_clean_event(peer);
}


static void
ngx_http_dynamic_update_upstream_clean_event(
        ngx_http_dynamic_update_upstream_server_t *peer)
{
    ngx_msec_t                                   t, tmp;
    ngx_pool_t                                  *pool;
    ngx_connection_t                            *c;
    ngx_http_dynamic_update_upstream_ctx_t      *ctx;
    ngx_http_dynamic_update_upstream_srv_conf_t *conf;

    conf = peer->conf;

    ctx = &peer->ctx;
    pool = ctx->pool;

    c = peer->pc.connection;

    if (c) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                "dynamic_update_upstream_clean_event: clean event: fd: %d", c->fd);

        ngx_close_connection(c);
        peer->pc.connection = NULL;
    }

    if (parser != NULL) {
        ngx_free(parser);
    }
    parser = NULL;

    if (pool != NULL) {
        ngx_destroy_pool(pool);
    }
    ctx->pool = NULL;

    if (!peer->update_ev.timer_set) {

        tmp = conf->update_interval;
        t = ngx_random() % 1000 + tmp;

        ngx_add_timer(&peer->update_ev, t);
    }

    if (peer->update_timeout_ev.timer_set) {
        ngx_del_timer(&peer->update_timeout_ev);
    }

    return;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_need_exit()
{
    if (ngx_terminate || ngx_exiting || ngx_quit) {
        ngx_http_dynamic_update_upstream_clear_all_events();

        return 1;
    }

    return NGX_OK;
}


static void
ngx_http_dynamic_update_upstream_clear_all_events()
{
    ngx_uint_t                                  i;
    ngx_connection_t                           *c;
    ngx_http_dynamic_update_upstream_server_t  *peer;

    static ngx_flag_t                has_cleared = 0;

    if (has_cleared || dynamic_upstream_ctx == NULL) {
        return;
    }

    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "[WARN]:"
            "dynamic_update_upstream_clear_all_events: on %P ", ngx_pid);

    has_cleared = 1;

    peer = dynamic_upstream_ctx->conf_server;

    for (i = 0; i < dynamic_upstream_ctx->upstream_num; i++) {

        if (peer[i].update_ev.timer_set) {
            ngx_del_timer(&peer[i].update_ev);
        }

        if (peer[i].update_timeout_ev.timer_set) {
            c = peer[i].pc.connection;
            if (c) {
                ngx_close_connection(c);
                peer->pc.connection = NULL;
            }
            ngx_del_timer(&peer[i].update_timeout_ev);
        }

    }

    if (parser != NULL) {
        ngx_free(parser);
    }
    parser = NULL;

    return;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_get_all(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server, char **conf_value)
{
    ngx_http_conf_client *client = ngx_http_create_client(cycle, conf_server);

    if (client == NULL) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "dynamic_update_upstream_get_all: http client create error");
        return NGX_ERROR;
    }

    ngx_int_t status = ngx_http_client_conn(client);
    if (status != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "dynamic_update_upstream_get_all: http client conn error");

        ngx_http_client_destroy(client);
        return NGX_ERROR;
    }

    char *response = NULL;

    ngx_http_client_send(client, conf_server);

    if (ngx_http_client_recv(client, &response, 0) <= 0) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "dynamic_update_upstream_get_all: http client recv fail");

        if (response != NULL) {
            ngx_free(response);
        }
        response = NULL;

        ngx_http_client_destroy(client);
        return NGX_ERROR;
    }

    ngx_http_client_destroy(client);

    *conf_value = response;

    return NGX_OK;
}


static ngx_http_conf_client *
ngx_http_create_client(ngx_cycle_t *cycle, ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    ngx_http_conf_client                         *client = NULL;
    ngx_http_upstream_server_t                   *us;
    ngx_http_dynamic_update_upstream_srv_conf_t  *conf;

    conf = conf_server->conf;
    us = &conf->us;

    client = ngx_calloc(sizeof(ngx_http_conf_client), cycle->log);
    if (client == NULL) {
        return NULL;
    }

    client->sd = -1;
    client->connected = 0;
    client->addr = *(struct sockaddr_in *)us->addrs[0].sockaddr;

    if((client->sd = socket(AF_INET,SOCK_STREAM,0)) == -1){
        ngx_free(client);
        client = NULL;

        return NULL;
    }

    struct timeval tv_timeout;
    tv_timeout.tv_sec = NGX_HTTP_SOCKET_TIMEOUT;
    tv_timeout.tv_usec = 0;

    if (setsockopt(client->sd, SOL_SOCKET, SO_SNDTIMEO, (void *) &tv_timeout, sizeof(struct timeval)) < 0) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "ngx_http_create_client: setsockopt SO_SNDTIMEO error");
        ngx_http_client_destroy(client);
        return NULL;
    }

    if (setsockopt(client->sd, SOL_SOCKET, SO_RCVTIMEO, (void *) &tv_timeout, sizeof(struct timeval)) < 0) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "ngx_http_create_client: setsockopt SO_RCVTIMEO error");
        ngx_http_client_destroy(client);
        return NULL;
    }

    return client;
}


static ngx_int_t
ngx_http_client_conn(ngx_http_conf_client *client) 
{
    if (connect(client->sd, (struct sockaddr *)&(client->addr), sizeof(struct sockaddr)) == -1) {
        return NGX_ERROR;
    }

    client->connected = 1;
    return NGX_OK;
}


static void 
ngx_http_client_destroy(ngx_http_conf_client *client) 
{
    close(client->sd);

    ngx_free(client);
    client = NULL;
}


static ngx_int_t 
ngx_http_client_send(ngx_http_conf_client *client, 
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    size_t       size = 0;
    ngx_int_t    tmp_send = 0;
    ngx_uint_t   send_num = 0;

    ngx_http_dynamic_update_upstream_srv_conf_t  *conf;

    conf = conf_server->conf;

    u_char request[NGX_BLOCK];
    ngx_memzero(request, NGX_BLOCK);
    ngx_sprintf(request, "GET %V?recurse HTTP/1.0\r\nHost: %V\r\nAccept: */*\r\n\r\n", 
            &conf->update_send, &conf->us.name);

    size = ngx_strlen(request);
    while(send_num < size) {
        tmp_send = send(client->sd, request + send_num, size - send_num, 0);
        /* TODO if tmp send is 0? */
        if (tmp_send < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                    "ngx_http_client_send: send byte %d", tmp_send);
            return NGX_ERROR;
        }

        send_num += tmp_send;
    }

    return send_num;
}


static ngx_int_t
ngx_http_client_recv(ngx_http_conf_client *client, char **data, int size) 
{
    ssize_t      recv_num = 0, tmp_recv = 0;
    char         buff[ngx_pagesize];
    char        *tmp_data;
    ngx_int_t    page_count = 0;

    *data = NULL;

    while(recv_num < size || size == 0) {  
        tmp_recv = recv(client->sd, buff, ngx_pagesize, 0);
        if (tmp_recv <= 0) {
            break;
        }

        recv_num += tmp_recv;
        if (*data == NULL) {
            *data = (char *) ngx_calloc(ngx_pagesize, ngx_cycle->log);
            if (*data == NULL) {
                return NGX_ERROR;
            }
            page_count++;
        }

        if (recv_num >= (ssize_t)(page_count * ngx_pagesize)) {
            tmp_data = *data;
            page_count++;

            *data = (char *) ngx_calloc(page_count * ngx_pagesize, ngx_cycle->log);
            if (*data == NULL) {
                return NGX_ERROR;
            }
            ngx_memcpy(*data, tmp_data, recv_num - tmp_recv);

            ngx_free(tmp_data);
            tmp_data = NULL;
        }

        ngx_memcpy(*data + recv_num - tmp_recv, buff, tmp_recv);
    }

    if (*data != NULL) {
        *(*data + recv_num) = '\0';
    }

    return recv_num;
}


static char *
ngx_http_dynamic_update_upstream_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_dynamic_update_upstream_show;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_show(ngx_http_request_t *r)
{
    ngx_buf_t                             *b;
    ngx_int_t                              rc, ret;
    ngx_str_t                             *host;
    ngx_uint_t                             i;
    ngx_chain_t                            out;
    ngx_http_upstream_rr_peers_t          *peers=NULL;
    ngx_http_upstream_srv_conf_t         **uscfp=NULL, *uscf=NULL;
    ngx_http_upstream_main_conf_t         *umcf;

    umcf = ngx_http_cycle_get_module_main_conf(ngx_cycle, ngx_http_upstream_module);

    uscfp = umcf->upstreams.elts;

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc; 
    }

    ngx_str_set(&r->headers_out.content_type, "text/plain");
    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;

        rc = ngx_http_send_header(r);

        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

    b = ngx_create_temp_buf(r->pool, NGX_PAGE_COUNT * ngx_pagesize);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    out.buf = b;
    out.next = NULL;

    host = &r->args;
    if (host->len == 0 || host->data == NULL) {

        b->last = ngx_snprintf(b->last,b->end - b->last, "Please input specific upstream name");
        goto end;
    }

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        if (uscfp[i]->host.len == host->len
            && ngx_strncasecmp(uscfp[i]->host.data, host->data, host->len) == 0) 
        {
            uscf = uscfp[i];
            break;
        }
    }

    if (i == umcf->upstreams.nelts) {

        b->last = ngx_snprintf(b->last,b->end - b->last, "The upstream name you input is not exited,"
                                                         "Please check and input again");
        goto end;
    }

    if (uscf->peer.data != NULL) {
        peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
    }

    b->last = ngx_snprintf(b->last,b->end - b->last, "Upstream name: %V;", host);
    b->last = ngx_snprintf(b->last,b->end - b->last, "Backend server counts: %d\n", peers->number);

    for (i = 0; i < peers->number; i++) {
        b->last = ngx_snprintf(b->last,b->end - b->last, "        server %V", &peers->peer[i].name);
        b->last = ngx_snprintf(b->last,b->end - b->last, " weight=%d", peers->peer[i].weight);
        b->last = ngx_snprintf(b->last,b->end - b->last, " max_fails=%d", peers->peer[i].max_fails);
        b->last = ngx_snprintf(b->last,b->end - b->last, " fail_timeout=%d;\n", peers->peer[i].fail_timeout);
    }

end:
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    b->last_buf = (r == r->main) ? 1 : 0;

    r->connection->buffered |= NGX_HTTP_WRITE_BUFFERED;
    ret = ngx_http_send_header(r);
    ret = ngx_http_output_filter(r, &out);
 
    return ret;
}
