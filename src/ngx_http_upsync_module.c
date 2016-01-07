/*
 * Copyright (C) 2015 Weibo Group Holding Limited
 * Copyright (C) 2015 Xiaokai Wang (xiaokai.wang@live.com)
 */


#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_config.h>

#include "ngx_http_upsync_module.h"


/* client for conf service */
typedef struct {
    ngx_int_t                 sd;
    ngx_int_t                 port;
    ngx_int_t                 connected;

    char                      ip[16];
    struct sockaddr_in        addr;
} ngx_http_conf_client;


typedef struct {
    u_char                           sockaddr[NGX_SOCKADDRLEN];

    ngx_int_t                        weight;
    ngx_uint_t                       max_fails;
    time_t                           fail_timeout;

    unsigned                         down:1;
    unsigned                         backup:1;
} ngx_http_upsync_conf_t;


#define NGX_HTTP_UPSYNC_CONSUL               0x0001


typedef ngx_int_t (*ngx_http_upsync_packet_init_pt)
    (void *upsync_server);
typedef ngx_int_t (*ngx_http_upsync_packet_parse_pt)
    (void *upsync_server);
typedef void (*ngx_http_upsync_packet_clean_pt)
    (void *upsync_server);


typedef struct {
    ngx_str_t                                name;

    ngx_uint_t                               upsync_type;

    ngx_event_handler_pt                     send_handler;
    ngx_event_handler_pt                     recv_handler;

    ngx_http_upsync_packet_init_pt           init;
    ngx_http_upsync_packet_parse_pt          parse;
    ngx_http_upsync_packet_clean_pt          clean;
} ngx_upsync_conf_t;


typedef struct {
    ngx_pool_t                      *pool;

    ngx_buf_t                        send;
    ngx_buf_t                        recv;

    ngx_buf_t                        body;

    ngx_array_t                      del_upstream;  /* ngx_http_upsync_conf_t */
    ngx_array_t                      add_upstream;

    ngx_array_t                      upstream_conf;
} ngx_http_upsync_ctx_t;


typedef struct {
    ngx_str_t                        upsync_host;
    ngx_int_t                        upsync_port;

    ngx_msec_t                       upsync_timeout;
    ngx_msec_t                       upsync_interval;

    ngx_uint_t                       strong_dependency;

    ngx_str_t                        upsync_send;
    ngx_str_t                        upsync_dump_path;

    ngx_open_file_t                 *conf_file;

    ngx_upsync_conf_t               *upsync_type_conf;

    ngx_http_upstream_server_t       conf_server;         /* conf server */
} ngx_http_upsync_srv_conf_t;


/* based on upstream conf, every unit upsync from consul */
typedef struct {
    ngx_str_t                                host;

    ngx_uint_t                               index;

    ngx_event_t                              upsync_ev;
    ngx_event_t                              upsync_timeout_ev;

    ngx_queue_t                              add_ev;
    ngx_queue_t                              delete_ev;

    ngx_shmtx_t                              upsync_accept_mutex;

    ngx_peer_connection_t                    pc;

    ngx_http_upsync_ctx_t                    ctx;

    ngx_http_upsync_srv_conf_t              *upscf;

    ngx_http_upstream_srv_conf_t            *uscf;
} ngx_http_upsync_server_t;


typedef struct {
    ngx_event_t                              delay_delete_ev;

    ngx_queue_t                              queue;

    time_t                                   start_sec;
    ngx_msec_t                               start_msec;

    void                                    *data;
} ngx_delay_event_t;


typedef struct {
    ngx_uint_t                               upstream_num;

    ngx_http_upsync_server_t                *upsync_server;
} ngx_http_upsync_main_conf_t;


/* http parser state */
typedef struct {
    u_char     status[3];

    char       headers[NGX_MAX_HEADERS][2][NGX_MAX_ELEMENT_SIZE];

    ngx_uint_t num_headers;

    enum { NONE=0, FIELD, VALUE } last_header;

    u_char     http_body[NGX_PAGE_SIZE * NGX_PAGE_NUMBER];
} ngx_http_state;


static ngx_upsync_conf_t *ngx_http_get_upsync_type_conf(ngx_str_t *str);
static char *ngx_http_upsync_server(ngx_conf_t *cf, 
    ngx_command_t *cmd, void *conf);
static char *ngx_http_upsync_set_conf_dump(ngx_conf_t *cf, 
    ngx_command_t *cmd, void *conf);

static void *ngx_http_upsync_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_upsync_create_srv_conf(ngx_conf_t *cf);
static char *ngx_http_upsync_init_main_conf(ngx_conf_t *cf, void *conf);
static char *ngx_http_upsync_init_srv_conf(ngx_conf_t *cf, void *conf, 
    ngx_uint_t num);

static void ngx_http_upsync_process(ngx_http_upsync_server_t *upsync_server);

static ngx_int_t ngx_http_upsync_init_process(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_upsync_init_module(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_upsync_init_shm_mutex(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_upsync_add_timers(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_upsync_init_peers(ngx_cycle_t *cycle,
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_upsync_parse_dump_file(
    ngx_http_upsync_server_t *upsync_server);

static void ngx_http_upsync_begin_handler(ngx_event_t *event);
static void ngx_http_upsync_connect_handler(ngx_event_t *event);
static void ngx_http_upsync_recv_handler(ngx_event_t *event);
static void ngx_http_upsync_send_handler(ngx_event_t *event);
static void ngx_http_upsync_timeout_handler(ngx_event_t *event);
static void ngx_http_upsync_clean_event(void *upsync_server);
static ngx_int_t ngx_http_upsync_parse_init(void *upsync_server);
static ngx_int_t ngx_http_upsync_dump_server(
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_upsync_init_server(ngx_event_t *event);

static ngx_int_t ngx_http_upsync_add_server(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_upsync_add_peer(ngx_cycle_t *cycle, 
    ngx_array_t *servers, ngx_http_upsync_server_t *upsync_server);
static void ngx_http_upsync_add_check(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server);

static ngx_int_t ngx_http_upsync_del_server(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_upsync_del_peer(ngx_cycle_t *cycle,
    ngx_http_upstream_server_t *us, ngx_http_upsync_server_t *upsync_server);
static void ngx_http_upsync_del_check(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server);

static ngx_int_t ngx_http_upsync_server_weight(ngx_cycle_t *cycle,
    ngx_http_upsync_server_t *upsync_server);

static void ngx_http_upsync_event_init(ngx_http_upstream_rr_peers_t *tmp_peers, 
    ngx_http_upsync_server_t *upsync_server, ngx_flag_t flag);

static ngx_int_t ngx_http_parser_init();
static void ngx_http_parser_execute(ngx_http_upsync_ctx_t *ctx);

static int ngx_http_status(http_parser *p, const char *buf, size_t len);
static int ngx_http_header_field_cb(http_parser *p, const char *buf, 
    size_t len);
static int ngx_http_header_value_cb(http_parser *p, const char *buf, 
    size_t len);
static int ngx_http_body(http_parser *p, const char *buf, size_t len);

static ngx_int_t ngx_http_upsync_check_index(
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_upsync_parse_json(void  *upsync_server);
static ngx_int_t ngx_http_upsync_check_key(u_char *key);
static void ngx_http_upsync_free(ngx_http_upstream_rr_peers_t *peers);

static void ngx_http_upsync_add_delay_delete(ngx_event_t *event);
static void ngx_http_upsync_del_delay_delete(ngx_event_t *event);

static ngx_int_t ngx_http_upsync_need_exit();
static void ngx_http_upsync_clear_all_events();

static ngx_int_t ngx_http_upsync_get_upstream(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server, char **conf_value);
static ngx_http_conf_client *ngx_http_create_client(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_client_conn(ngx_http_conf_client *client);
static void ngx_http_client_destroy(ngx_http_conf_client *client);
static ngx_int_t ngx_http_client_send(ngx_http_conf_client *client, 
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_client_recv(ngx_http_conf_client *client, 
    char **data, int size);

static char *ngx_http_upsync_set(ngx_conf_t *cf, ngx_command_t *cmd, 
    void *conf);
static ngx_int_t ngx_http_upsync_show(ngx_http_request_t *r);


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

ngx_atomic_t   upsync_shared_created0;
ngx_atomic_t  *upsync_shared_created = &upsync_shared_created0;

static http_parser *parser = NULL;
static ngx_http_state state;

static ngx_http_upsync_main_conf_t  *upsync_ctx = NULL;

static ngx_command_t  ngx_http_upsync_commands[] = {

    {  ngx_string("upsync"),
        NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
        ngx_http_upsync_server,
        NGX_HTTP_SRV_CONF_OFFSET,
        0,
        NULL },

    {  ngx_string("upsync_dump_path"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_http_upsync_set_conf_dump,
        NGX_HTTP_SRV_CONF_OFFSET,
        0,
        NULL },

    {  ngx_string("upstream_show"),
        NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
        ngx_http_upsync_set,
        0,
        0,
        NULL },

    ngx_null_command
};


static ngx_http_module_t  ngx_http_upsync_module_ctx = {
    NULL,                                       /* preconfiguration */
    NULL,                                       /* postconfiguration */

    ngx_http_upsync_create_main_conf,           /* create main configuration */
    ngx_http_upsync_init_main_conf,             /* init main configuration */

    ngx_http_upsync_create_srv_conf,            /* create server configuration */
    NULL,                                       /* merge server configuration */

    NULL,                                       /* create location configuration */
    NULL                                        /* merge main configuration */
};


ngx_module_t  ngx_http_upsync_module = {
    NGX_MODULE_V1,
    &ngx_http_upsync_module_ctx,                /* module context */
    ngx_http_upsync_commands,                   /* module directives */
    NGX_HTTP_MODULE,                            /* module type */
    NULL,                                       /* init master */
    ngx_http_upsync_init_module,                /* init module */
    ngx_http_upsync_init_process,               /* init process */
    NULL,                                       /* init thread */
    NULL,                                       /* exit thread */
    NULL,                                       /* exit process */
    NULL,                                       /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_upsync_conf_t  ngx_upsync_types[] = {

    { ngx_string("consul"),
      NGX_HTTP_UPSYNC_CONSUL,
      ngx_http_upsync_send_handler,
      ngx_http_upsync_recv_handler,
      ngx_http_upsync_parse_init,
      ngx_http_upsync_parse_json,
      ngx_http_upsync_clean_event },

    { ngx_null_string,
      0,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL }
};


static char *
ngx_http_upsync_server(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    u_char                             *p = NULL;
    time_t                              upsync_timeout = 0, upsync_interval = 0;
    ngx_str_t                          *value, s;
    ngx_url_t                           u;
    ngx_uint_t                          i, strong_dependency = 0;
    ngx_http_upstream_server_t         *conf_server;
    ngx_http_upsync_srv_conf_t         *upscf;

    value = cf->args->elts;

    upscf = ngx_http_conf_get_module_srv_conf(cf,
                                              ngx_http_upsync_module);
    conf_server = &upscf->conf_server;

    for (i = 2; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "upsync_timeout=", 15) == 0) {

            s.len = value[i].len - 15;
            s.data = &value[i].data[15];

            upsync_timeout = ngx_parse_time(&s, 0);
            if (upsync_timeout == (time_t) NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "upsync_server: invalid parameter:\"%V\"", 
                                   &value[i]);
                goto invalid;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "upsync_interval=", 16) == 0) {

            s.len = value[i].len - 16;
            s.data = &value[i].data[16];

            upsync_interval = ngx_parse_time(&s, 0);
            if (upsync_interval == (time_t) NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "upsync_server: invalid parameter: \"%V\"", 
                                   &value[i]);
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

        if (ngx_strncmp(value[i].data, "upsync_type=", 12) == 0) {
            s.len = value[i].len - 12;
            s.data = value[i].data + 12;

            upscf->upsync_type_conf = ngx_http_get_upsync_type_conf(&s);
            if (upscf->upsync_type_conf == NULL) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "upsync_server: upsync_type invalid para");
                goto invalid;
            }

            continue;
        }

        goto invalid;
    }

    if (upsync_interval != 0) {
        upscf->upsync_interval = upsync_interval;
    }
    if (upsync_timeout != 0) {
        upscf->upsync_timeout = upsync_timeout;
    }
    if (strong_dependency != 0) {
        upscf->strong_dependency = strong_dependency;
    }
    if (upscf->upsync_type_conf == NGX_CONF_UNSET_PTR) {
         ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "upsync_server: upsync_type cannt be null");
          goto invalid;
    }

    ngx_memzero(&u, sizeof(ngx_url_t));

    p = (u_char *)ngx_strchr(value[1].data, '/');
    if (p == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "upsync_server: "
                           "please input conf_server upstream key in upstream");
        return NGX_CONF_ERROR;
    }
    upscf->upsync_send.data = p;
    upscf->upsync_send.len = value[1].len - (p - value[1].data);

    u.url.data = value[1].data;
    u.url.len = p - value[1].data;

    p = (u_char *)ngx_strchr(value[1].data, ':');
    if (p != NULL) {
        upscf->upsync_host.data = value[1].data;
        upscf->upsync_host.len = p - value[1].data;

        upscf->upsync_port = ngx_atoi(p + 1, upscf->upsync_send.data - p - 1);
        if (upscf->upsync_port < 1 || upscf->upsync_port > 65535) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "upsync_server: "
                               "conf server port is invalid");
            return NGX_CONF_ERROR;
        }

    } else {
        upscf->upsync_host.data = value[1].data;
        upscf->upsync_host.len = u.url.len;

        upscf->upsync_port = 80;
    }

    u.default_port = 80;
    if (ngx_parse_url(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "upsync_server: "
                               "%s in upstream \"%V\"", u.err, &u.url);
        }
        return NGX_CONF_ERROR;
    }

    conf_server->name = u.url;
    conf_server->addrs = u.addrs;
    conf_server->naddrs = u.naddrs;
    conf_server->weight = 1;
    conf_server->max_fails = 1;
    conf_server->fail_timeout = 10;

    return NGX_CONF_OK;

invalid:

    return NGX_CONF_ERROR;
}


static ngx_upsync_conf_t *
ngx_http_get_upsync_type_conf(ngx_str_t *str)
{
    ngx_uint_t  i;

    for (i = 0; /* void */ ; i++) {

        if (ngx_upsync_types[i].upsync_type == 0) {
            break;
        }

        if (str->len != ngx_upsync_types[i].name.len) {
            continue;
        }

        if (ngx_strncmp(str->data, ngx_upsync_types[i].name.data,
                        str->len) == 0)
        {
            return &ngx_upsync_types[i];
        }
    }

    return NULL;
}


static char *
ngx_http_upsync_set_conf_dump(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                         *value;
    ngx_http_upsync_srv_conf_t        *upscf;

    upscf = ngx_http_conf_get_module_srv_conf(cf,
                                              ngx_http_upsync_module);
    value = cf->args->elts;

    upscf->upsync_dump_path = value[1]; 
    if (upscf->upsync_dump_path.len == NGX_CONF_UNSET_SIZE) {
        return NGX_CONF_ERROR; 
    }

    upscf->conf_file = ngx_conf_open_file(cf->cycle, &value[1]); 
    if (upscf->conf_file == NULL) {
        return NGX_CONF_ERROR; 
    }

    return NGX_CONF_OK;
}


static void
ngx_http_upsync_process(ngx_http_upsync_server_t *upsync_server)
{
    ngx_uint_t                   add_flag = 0, del_flag = 0, weight_flag = 0;
    ngx_upsync_conf_t           *upsync_type_conf;
    ngx_http_upsync_ctx_t       *ctx;

    ctx = &upsync_server->ctx;
    upsync_type_conf = upsync_server->upscf->upsync_type_conf;

    if (ngx_http_upsync_check_index(upsync_server) == NGX_ERROR) {
        return;
    }

    if (upsync_type_conf->parse(upsync_server) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_process: parse json error");
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG, ngx_cycle->log, 0,
                   "upsync_process: parse json succeed");

    ngx_http_upsync_add_check((ngx_cycle_t *)ngx_cycle, upsync_server);
    if (ctx->add_upstream.nelts > 0) {

        if (ngx_http_upsync_add_server((ngx_cycle_t *)ngx_cycle, 
                                       upsync_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_process: upstream add server error");
            return;
        }
        add_flag = 1;
    }

    ngx_http_upsync_del_check((ngx_cycle_t *)ngx_cycle, upsync_server);
    if (ctx->del_upstream.nelts > 0) {

        if (ngx_http_upsync_del_server((ngx_cycle_t *)ngx_cycle, 
                                       upsync_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_process: upstream del server error");
            return;
        }
        del_flag = 1;
    }

    if (!add_flag && !del_flag) {
        if (ngx_http_upsync_server_weight((ngx_cycle_t *)ngx_cycle, 
                                           upsync_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_process: upstream change peer weight error");
            return;
        }
        weight_flag = 1;
    }

    if (add_flag || del_flag || weight_flag) {
        if (ngx_shmtx_trylock(&upsync_server->upsync_accept_mutex)) {

            ngx_http_upsync_dump_server(upsync_server);
            ngx_shmtx_unlock(&upsync_server->upsync_accept_mutex);
        }
    }

    return;
}


static ngx_int_t
ngx_http_upsync_check_index(ngx_http_upsync_server_t *upsync_server)
{
    char                        *p;
    ngx_uint_t                   i, index = 0;
    ngx_upsync_conf_t           *upsync_type_conf;

    upsync_type_conf = upsync_server->upscf->upsync_type_conf;

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL) {
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

        if (index == upsync_server->index) {
            ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                          "upsync_check_index: upstream index not change: %V",
                          &upsync_server->upscf->upsync_dump_path);
            return NGX_ERROR;

        } else {
            upsync_server->index = index;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_add_server(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server)
{
    u_char                          *port, *p, *last, *pp;
    ngx_int_t                        n;
    ngx_uint_t                       i;
    ngx_addr_t                      *addrs;
    ngx_array_t                      servers;  /* ngx_http_upstream_server_t */
    ngx_http_upsync_ctx_t           *ctx;
    ngx_http_upsync_conf_t          *conf;
    ngx_http_upstream_server_t      *server;

    struct sockaddr_in  *sin;

    ctx = &upsync_server->ctx;

    if (ngx_array_init(&servers, ctx->pool, 16, sizeof(*server)) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_add_server: alloc error");
        return NGX_ERROR;
    }

    for (i = 0; i < ctx->add_upstream.nelts; i++) {
        conf = (ngx_http_upsync_conf_t *)ctx->add_upstream.elts + i;

        p = conf->sockaddr;
        last = p + ngx_strlen(p);

        port = ngx_strlchr(p, last, ':');
        if (port == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                          "upsync_add_server: has no port in %s", p);
            continue;
        }

        n = ngx_atoi(port + 1, last - port - 1);
        if (n < 1 || n > 65535) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                          "upsync_add_server: invalid port in %s", p);
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
                          "upsync_add_server: invalid ip in %s", p);
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
        if (ngx_http_upsync_add_peer(cycle, &servers, 
                                     upsync_server) != NGX_OK) {
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
ngx_http_upsync_add_peer(ngx_cycle_t *cycle,
    ngx_array_t *servers, ngx_http_upsync_server_t *upsync_server)
{
    ngx_uint_t                     i=0, n=0, w=0, m=0;
    ngx_http_upstream_server_t    *server = NULL;
    ngx_http_upstream_rr_peers_t  *peers = NULL, *tmp_peers = NULL;
    ngx_http_upstream_srv_conf_t  *uscf;

    uscf = upsync_server->uscf;

    if (servers->nelts < 1) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_add_peer: no servers to add \"%V\"", &uscf->host);
        return NGX_ERROR;
    }

    if (uscf->peer.data != NULL) {
        tmp_peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
    }

    if (tmp_peers && servers->nelts >= 1) {
        n = tmp_peers->number + servers->nelts;

        peers = ngx_calloc(sizeof(ngx_http_upstream_rr_peers_t)
                           + sizeof(ngx_http_upstream_rr_peer_t) * (n - 1), 
                           cycle->log);
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
            ngx_uint_t index;
            index = ngx_http_upstream_check_add_dynamic_peer(cycle->pool, 
                                                             uscf, server->addrs);
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

        ngx_http_upsync_event_init(tmp_peers, upsync_server, NGX_ADD);
    }

    return NGX_OK;

invalid:
    ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                  "upsync_add_peer: add failed \"%V\"", &uscf->host);

    if (peers != NULL) {
        ngx_http_upsync_free(peers);
    }
    peers = NULL;

    uscf->peer.data = tmp_peers;

    return NGX_ERROR;
}


static void
ngx_http_upsync_add_check(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server)
{
    ngx_uint_t                          i, j, len;
    ngx_http_upsync_ctx_t              *ctx;
    ngx_http_upsync_conf_t             *upstream_conf, *add_upstream;
    ngx_http_upstream_rr_peers_t       *peers = NULL;
    ngx_http_upstream_srv_conf_t       *uscf;

    ctx = &upsync_server->ctx;

    if (ngx_array_init(&ctx->add_upstream, ctx->pool, 16,
                       sizeof(*add_upstream)) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_add_check: alloc error");
        return;
    }

    uscf = upsync_server->uscf;
    if (uscf->peer.data != NULL) {
        peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    } else {
        return;
    }

    len = ctx->upstream_conf.nelts;
    for (i = 0; i < len; i++) {
        upstream_conf = (ngx_http_upsync_conf_t *)ctx->upstream_conf.elts + i;

        for (j = 0; j < peers->number; j++) {

            if (ngx_memcmp(peers->peer[j].name.data, 
                           upstream_conf->sockaddr, peers->peer[j].name.len) 
                    == 0) 
            {
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
ngx_http_upsync_del_server(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server)
{
    u_char                             *port, *p, *last, *pp;
    ngx_int_t                           n, j;
    ngx_uint_t                          i;
    ngx_pool_t                         *pool;
    ngx_addr_t                         *addrs;
    ngx_http_upsync_ctx_t              *ctx;
    ngx_http_upsync_conf_t             *conf;
    ngx_http_upstream_server_t          us;

    struct sockaddr_in  *sin;

    ctx = &upsync_server->ctx;
    pool = ctx->pool;

    ngx_memzero(&us, sizeof(ngx_http_upstream_server_t));

    addrs = ngx_pcalloc(pool, ctx->del_upstream.nelts * sizeof(ngx_addr_t));
    if (addrs == NULL) {
        return NGX_ERROR;
    }

    for (i = 0, j = 0; i < ctx->del_upstream.nelts; i++, j++) {
        conf = (ngx_http_upsync_conf_t *)ctx->del_upstream.elts + i;

        p = conf->sockaddr;
        last = p + ngx_strlen(p);

        port = ngx_strlchr(p, last, ':');
        if(port == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                          "upsync_del_server: has no port in %s", p);
            j--;
            continue;
        }

        n = ngx_atoi(port + 1, last - port - 1);
        if (n < 1 || n > 65535) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                          "upsync_del_server: invalid port in %s", p);
            j--;
            continue;
        }

        sin = ngx_pcalloc(pool, sizeof(struct sockaddr_in));
        if (sin == NULL) {
            return NGX_ERROR;
        }

        sin->sin_family = AF_INET;
        sin->sin_port = htons((in_port_t) n);
        sin->sin_addr.s_addr = ngx_inet_addr(p, port - p);

        if (sin->sin_addr.s_addr == INADDR_NONE) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                          "upsync_del_server: invalid ip in %s", p);
            j--;
            continue;
        }

        addrs[j].sockaddr = (struct sockaddr *) sin;
        addrs[j].socklen = sizeof(struct sockaddr_in);

        pp = ngx_pcalloc(pool, last - p);
        if (pp == NULL) {
            return NGX_ERROR;
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
        if (ngx_http_upsync_del_peer(cycle, &us, 
                                     upsync_server) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_del_peer(ngx_cycle_t *cycle,
    ngx_http_upstream_server_t *us, ngx_http_upsync_server_t *upsync_server)
{
    ngx_uint_t                          i, j, n=0, w=0, len=0;
    ngx_http_upstream_rr_peers_t       *peers = NULL, *tmp_peers = NULL;
    ngx_http_upstream_srv_conf_t       *uscf;

    len = sizeof(struct sockaddr);

    uscf = upsync_server->uscf;
    if (uscf->peer.data != NULL) {
        tmp_peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
    }

    if (tmp_peers->number <= us->naddrs) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "[WARN]:"
                      "upsync_del_peer: upstream \"%V\" cannt delete all", 
                      &uscf->host);
        return NGX_OK;
    }

    if (tmp_peers) {
        n = tmp_peers->number - us->naddrs;
        w = tmp_peers->total_weight;

        peers = ngx_calloc(sizeof(ngx_http_upstream_rr_peers_t)
                           + sizeof(ngx_http_upstream_rr_peer_t) * (n - 1), 
                           cycle->log);
        if (peers == NULL) {
            goto invalid;
        }

        n = 0;
        for (i = 0; i < tmp_peers->number; i++) {
            for (j = 0; j < us->naddrs; j++) {
                if (ngx_memn2cmp((u_char *) tmp_peers->peer[i].sockaddr, 
                                 (u_char *) us->addrs[j].sockaddr, len, len) 
                        == 0) 
                {

#if (NGX_HTTP_UPSTREAM_CHECK) 
                    ngx_http_upstream_check_delete_dynamic_peer(
                                                tmp_peers->name, &us->addrs[j]);
#endif
                    w -= tmp_peers->peer[i].weight;
                    tmp_peers->peer[i].down = NGX_MAX_VALUE;

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

        ngx_http_upsync_event_init(tmp_peers, upsync_server, NGX_DEL);
    }

    return NGX_OK;

invalid:
    ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                  "upsync_del_peer: del failed \"%V\"", &uscf->host);

    if (peers != NULL) {
        ngx_http_upsync_free(peers);
    }
    peers = NULL;

    uscf->peer.data = tmp_peers;

    return NGX_ERROR;
}


static void
ngx_http_upsync_del_check(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server)
{
    ngx_uint_t                         i, j, len;
    ngx_http_upsync_ctx_t             *ctx;
    ngx_http_upsync_conf_t            *upstream_conf, *del_upstream;
    ngx_http_upstream_rr_peers_t      *peers = NULL;
    ngx_http_upstream_srv_conf_t      *uscf;

    ctx = &upsync_server->ctx;

    if (ngx_array_init(&ctx->del_upstream, ctx->pool, 16,
                       sizeof(*del_upstream)) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_del_check: alloc error");
        return;
    }

    uscf = upsync_server->uscf;
    if (uscf->peer.data != NULL) {
        peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    } else {
        return;
    }

    len = ctx->upstream_conf.nelts;
    for (i = 0; i < peers->number; i++) {
        for (j = 0; j < len; j++) {

            upstream_conf = (ngx_http_upsync_conf_t *)ctx->upstream_conf.elts + j;
            if (ngx_memcmp(peers->peer[i].name.data, 
                           upstream_conf->sockaddr, peers->peer[i].name.len) 
                    == 0) 
            {
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
ngx_http_upsync_server_weight(ngx_cycle_t *cycle,
    ngx_http_upsync_server_t *upsync_server)
{
    ngx_uint_t                           i, j, w=0, len=0;
    ngx_http_upsync_ctx_t               *ctx;
    ngx_http_upsync_conf_t              *upstream_conf;
    ngx_http_upstream_srv_conf_t        *uscf;
    ngx_http_upstream_rr_peers_t        *peers = NULL;

    ctx = &upsync_server->ctx;

    uscf = upsync_server->uscf;

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

            upstream_conf = (ngx_http_upsync_conf_t *)ctx->upstream_conf.elts + j;
            if (ngx_memcmp(peers->peer[i].name.data, 
                           upstream_conf->sockaddr, peers->peer[i].name.len) 
                    == 0) 
            {
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
ngx_http_upsync_parse_json(void *data)
{
    u_char                          *p;
    ngx_buf_t                       *buf;
    ngx_int_t                       max_fails=2, backup=0, down=0;
    ngx_str_t                       src, dst;
    ngx_http_upsync_ctx_t          *ctx;
    ngx_http_upsync_conf_t         *upstream_conf = NULL;
    ngx_http_upsync_server_t       *upsync_server = data;

    ctx = &upsync_server->ctx;
    buf = &ctx->body;

    src.len = 0, src.data = NULL;
    dst.len = 0, dst.data = NULL;

    cJSON *root = cJSON_Parse((char *)buf->pos);
    if (root == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_parse_json: root error");
        return NGX_ERROR;
    }

    if (ngx_array_init(&ctx->upstream_conf, ctx->pool, 16,
                       sizeof(*upstream_conf)) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_parse_json: array init error");
        return NGX_ERROR;
    }

    cJSON *server_next;
    for (server_next = root->child; server_next != NULL; 
            server_next = server_next->next) {

        cJSON *temp1 = cJSON_GetObjectItem(server_next, "Key");
        if (temp1 != NULL && temp1->valuestring != NULL) {
            p = (u_char *)ngx_strrchr(temp1->valuestring, '/');
            if (ngx_http_upsync_check_key(p) != NGX_OK) {
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
                              "upsync_parse_json: parse attribute json failed,"
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
                          "upsync_parse_json: \"weight\" value is invalid"
                          " and seting to 1");
            upstream_conf->weight = 1;
        }

        if (max_fails < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"max_fails\" value is invalid"
                          " and seting to 2");
        } else {
            upstream_conf->max_fails = (ngx_uint_t)max_fails;
        }

        if (upstream_conf->fail_timeout < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"fail_timeout\" value is invalid"
                          " and seting to 10");
            upstream_conf->fail_timeout = 10;
        }

        if (down != 1 && down != 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"down\" value is invalid"
                          " and seting to 0");
        } else {
            upstream_conf->down = (ngx_uint_t)down;
        }

        if (backup != 1 && backup != 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"backup\" value is invalid"
                          " and seting to 0");
        } else {
            upstream_conf->backup = (ngx_uint_t)backup;
        }

    }
    cJSON_Delete(root);

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_check_key(u_char *key)
{
    u_char          *last, *ip_p, *port_p;
    ngx_int_t        port;

    port_p = (u_char *)ngx_strchr(key, ':');
    if (port_p == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, 
                      "upsync_check_key: has no port in %s", key);
        return NGX_ERROR;
    }

    ip_p = key + 1;
    if (ngx_inet_addr(ip_p, port_p - ip_p) == INADDR_NONE) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, 
                      "upsync_check_key: invalid ip in %s", key);
        return NGX_ERROR;
    }

    last = ip_p + ngx_strlen(ip_p);
    port = ngx_atoi(port_p + 1, last - port_p - 1);
    if (port < 1 || port > 65535) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, 
                      "upsync_check_key: invalid port in %s", key);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_http_upsync_free(ngx_http_upstream_rr_peers_t *peers)
{
    if (peers != NULL) {
        ngx_free(peers);
        peers = NULL;
    }

    return;
}


static void *
ngx_http_upsync_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_upsync_main_conf_t       *upmcf;

    upmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upsync_main_conf_t));
    if (upmcf == NULL) {
        return NULL;
    }

    upmcf->upstream_num = NGX_CONF_UNSET_UINT;
    upmcf->upsync_server = NGX_CONF_UNSET_PTR;

    return upmcf;
}


static char *
ngx_http_upsync_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_uint_t                                     i;
    ngx_http_upsync_main_conf_t                   *upmcf = conf;
    ngx_http_upstream_srv_conf_t                 **uscfp;
    ngx_http_upstream_main_conf_t                 *umcf;

    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);

    upmcf->upsync_server = ngx_pcalloc(cf->pool, 
                      umcf->upstreams.nelts * sizeof(ngx_http_upsync_server_t));

    if (upmcf->upsync_server == NULL) {
        return NGX_CONF_ERROR;
    }

    upmcf->upstream_num = 0;
    upsync_ctx = upmcf;
    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        if (ngx_http_upsync_init_srv_conf(cf, uscfp[i], i) != NGX_OK) {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


static void *
ngx_http_upsync_create_srv_conf(ngx_conf_t *cf)
{
    ngx_http_upsync_srv_conf_t  *upscf;

    upscf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upsync_srv_conf_t));
    if (upscf == NULL) {
        return NULL;
    }

    upscf->upsync_host.len = NGX_CONF_UNSET_SIZE;
    upscf->upsync_host.data = NGX_CONF_UNSET_PTR;

    upscf->upsync_port = NGX_CONF_UNSET;

    upscf->upsync_dump_path.len = NGX_CONF_UNSET_SIZE;
    upscf->upsync_dump_path.data = NGX_CONF_UNSET_PTR;

    upscf->upsync_timeout = NGX_CONF_UNSET_MSEC;
    upscf->upsync_interval = NGX_CONF_UNSET_MSEC;

    upscf->strong_dependency = NGX_CONF_UNSET_UINT;

    upscf->conf_file = NGX_CONF_UNSET_PTR;

    upscf->upsync_type_conf = NGX_CONF_UNSET_PTR;

    ngx_memzero(&upscf->conf_server, sizeof(upscf->conf_server));

    return upscf;
}


static char *
ngx_http_upsync_init_srv_conf(ngx_conf_t *cf, void *conf, ngx_uint_t num)
{
    u_char                                      *buf;
    ngx_http_upsync_server_t                    *upsync_server;
    ngx_http_upsync_srv_conf_t                  *upscf;
    ngx_http_upstream_srv_conf_t                *uscf = conf;

    if (uscf->srv_conf == NULL) {
        return NGX_CONF_OK;
    }

    upscf = ngx_http_conf_upstream_srv_conf(uscf, ngx_http_upsync_module);
    if (upscf->upsync_host.data == NGX_CONF_UNSET_PTR 
        && upscf->upsync_host.len == NGX_CONF_UNSET_SIZE) {
        return NGX_CONF_OK;
    }

    upsync_ctx->upstream_num++;

    upsync_server = &upsync_ctx->upsync_server[upsync_ctx->upstream_num - 1];
    if (upsync_server == NULL) {
        return NGX_CONF_ERROR;
    }

    if (upscf->upsync_timeout == NGX_CONF_UNSET_MSEC) {
        upscf->upsync_timeout = 1000 * 60 * 6;
    }

    if (upscf->upsync_interval == NGX_CONF_UNSET_MSEC) {
        upscf->upsync_interval = 1000 * 5;
    }

    if (upscf->strong_dependency == NGX_CONF_UNSET_UINT) {
        upscf->strong_dependency = 0;
    }

    if (upscf->upsync_dump_path.len == NGX_CONF_UNSET_SIZE) {
        buf = ngx_pcalloc(cf->pool, 
                          ngx_strlen("/tmp/upstream_.conf") + uscf->host.len + 1);
        ngx_sprintf(buf, "/tmp/upstream_%V.conf", &uscf->host);

        upscf->upsync_dump_path.data = buf;
        upscf->upsync_dump_path.len = ngx_strlen("/tmp/upstream_.conf")
                                      + uscf->host.len;
    }

    upscf->conf_file = ngx_conf_open_file(cf->cycle, &upscf->upsync_dump_path); 
    if (upscf->conf_file == NULL) {
        return NGX_CONF_ERROR; 
    }

    upsync_server->index = 0;

    upsync_server->upscf = upscf;
    upsync_server->uscf = uscf;

    upsync_server->host.len = uscf->host.len;
    upsync_server->host.data = uscf->host.data;

    return NGX_CONF_OK;
}


static ngx_int_t 
ngx_http_upsync_init_module(ngx_cycle_t *cycle)
{
    ngx_uint_t                       i;
    ngx_http_upsync_server_t        *upsync_server;
    ngx_http_upsync_srv_conf_t      *upscf;

    upsync_server = upsync_ctx->upsync_server;

    if (ngx_http_upsync_init_shm_mutex(cycle) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, "upsync_init_module:"
                      " init shm mutex failed");
        return NGX_ERROR;
    }

    for (i = 0; i < upsync_ctx->upstream_num; i++) {

        upscf = upsync_server[i].upscf;
        if (upscf->conf_file->fd != NGX_INVALID_FILE) {
            ngx_close_file(upscf->conf_file->fd);
            upscf->conf_file->fd = NGX_INVALID_FILE;
        }
        ngx_change_file_access(upscf->upsync_dump_path.data, 
                               S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH|S_IWOTH);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_init_shm_mutex(ngx_cycle_t *cycle)
{
    u_char                                *shared, *file;
    size_t                                 size, cl;
    ngx_shm_t                              shm;
    ngx_uint_t                             i;
    ngx_http_upsync_server_t              *upsync_server;

    upsync_server = upsync_ctx->upsync_server;

    if (*upsync_shared_created) {
        shm.size = 128 * (*upsync_shared_created);
        shm.log = cycle->log;
        shm.addr = (u_char *)(upsync_shared_created);
        shm.name.len = sizeof("ngx_upsync_shared_zone");
        shm.name.data = (u_char *)"ngx_upsync_shared_zone";

        ngx_shm_free(&shm);
    }

    /* cl should be equal to or greater than cache line size 
       shared created flag
       upsync_accept_mutex for every upstream 
    */

    cl = 128;
    size = cl                                       
         + cl * upsync_ctx->upstream_num;

    shm.size = size;
    shm.log = cycle->log;
    shm.name.len = sizeof("ngx_upsync_shared_zone");
    shm.name.data = (u_char *)"ngx_upsync_shared_zone";

    if (ngx_shm_alloc(&shm) != NGX_OK) {
        return NGX_ERROR;
    }
    shared = shm.addr;

    upsync_shared_created = (ngx_atomic_t *)shared;

    for (i = 0; i < upsync_ctx->upstream_num; i++) {

#if (NGX_HAVE_ATOMIC_OPS)

        file = NULL;

#else

        file = ngx_pcalloc(cycle->pool, 
                           cycle->lock_file.len + ngx_strlen("upsync") + 3);
        if (file == NULL) {
            return NGX_ERROR;
        }

        (void) ngx_sprintf(file, "%V%s%d%Z", &ngx_cycle->lock_file, "upsync", i);

#endif

        if (ngx_shmtx_create(&upsync_server[i].upsync_accept_mutex, 
                             (ngx_shmtx_sh_t *)(shared + (i + 1) * cl), file) 
                != NGX_OK) 
        {
            return NGX_ERROR;
        }
    }

    ngx_atomic_cmp_set(upsync_shared_created, *upsync_shared_created, 
                       upsync_ctx->upstream_num);

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_init_process(ngx_cycle_t *cycle)
{
    char                                *conf_value = NULL;
    ngx_int_t                            status;
    ngx_uint_t                           i, j;
    ngx_pool_t                          *pool;
    ngx_upsync_conf_t                   *upsync_type_conf;
    ngx_http_upsync_ctx_t               *ctx;
    ngx_http_upsync_server_t            *upsync_server;

    upsync_server = upsync_ctx->upsync_server;

    for (i = 0; i < upsync_ctx->upstream_num; i++) {
        upsync_type_conf = upsync_server[i].upscf->upsync_type_conf;

        ctx = &upsync_server[i].ctx;
        ngx_memzero(ctx, sizeof(*ctx));

        ngx_http_upsync_init_peers(cycle, &upsync_server[i]);

        pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, ngx_cycle->log);
        if (pool == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                          "upsync_init_process: recv no enough memory");
            return NGX_ERROR;
        }
        ctx->pool = pool;

        for (j = 0; j < NGX_HTTP_RETRY_TIMES; j++) {
            status = ngx_http_upsync_get_upstream(cycle, 
                                                  &upsync_server[i], &conf_value);
            if (status == NGX_OK) {
                break;
            }
        }

        if (status != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                          "upsync_init_process: pull upstream conf failed");

            if (upsync_server[i].upscf->strong_dependency == 0) {
                ngx_http_upsync_parse_dump_file(&upsync_server[i]);

                ngx_destroy_pool(pool);
                ctx->pool = NULL;

                continue;
            }
            return NGX_ERROR;
        }

        ctx->recv.pos = (u_char *)conf_value;
        ctx->recv.last = (u_char *)(conf_value + ngx_strlen(conf_value));
        ctx->recv.end = ctx->recv.last;

        if (upsync_type_conf->init(&upsync_server[i]) == NGX_ERROR) {
            ngx_free(conf_value);
            conf_value = NULL;

            ngx_destroy_pool(pool);
            ctx->pool = NULL;

            continue;
        }

        ngx_http_upsync_process(&upsync_server[i]);

        ngx_free(conf_value);
        conf_value = NULL;

        ngx_destroy_pool(pool);
        ctx->pool = NULL;
    }

    ngx_http_upsync_add_timers(cycle);

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_init_peers(ngx_cycle_t *cycle,
    ngx_http_upsync_server_t *upsync_server)
{
    ngx_uint_t                          i, n, len;
    ngx_http_upstream_rr_peers_t       *peers = NULL, *tmp_peers = NULL;
    ngx_http_upstream_srv_conf_t       *uscf;

    uscf = upsync_server->uscf;

    u_char *namep = NULL;
    struct sockaddr *saddr = NULL;
    len = sizeof(struct sockaddr);

    ngx_queue_init(&upsync_server->add_ev);
    ngx_queue_init(&upsync_server->delete_ev);

    if (uscf->peer.data != NULL) {
        tmp_peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
    }

    if (tmp_peers) {
        n = tmp_peers->number;

        peers = ngx_calloc(sizeof(ngx_http_upstream_rr_peers_t)
                           + sizeof(ngx_http_upstream_rr_peer_t) * (n - 1), 
                           cycle->log);
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
                  "upsync_init_peers: copy failed \"%V\"", &uscf->host);

    if (peers != NULL) {
        ngx_http_upsync_free(peers);
    }
    uscf->peer.data = tmp_peers;

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_upsync_parse_dump_file(ngx_http_upsync_server_t *upsync_server)
{
    char                        *prev = NULL, *delimiter = NULL, read_line[1024];
    ngx_int_t                    max_fails;
    ngx_str_t                    s;
    ngx_http_upsync_ctx_t       *ctx;
    ngx_http_upsync_conf_t      *upstream_conf = NULL;
    ngx_http_upsync_srv_conf_t  *upscf = NULL;

    ctx = &upsync_server->ctx;
    upscf = upsync_server->upscf;

    FILE *fp = ngx_fopen((char *)upscf->upsync_dump_path.data, "r");
    if (fp == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_parse_dump_file: open dump file \"%V\" failed", 
                      &upscf->upsync_dump_path);
        return NGX_ERROR;
    }

    if (ngx_array_init(&ctx->upstream_conf, ctx->pool, 16,
                       sizeof(*upstream_conf)) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_parse_dump_file: array init error");
        return NGX_ERROR;
    }

    while (ngx_fgets(read_line, 1024, fp) != NULL) {

        prev = read_line;
        while(*prev != ';' && *prev != '\0') {

            if (ngx_strncmp(prev, "server", 6) == 0) {
                prev += 7;
                delimiter = ngx_strchr(prev, ' ');
                if (delimiter == NULL) {
                    delimiter = ngx_strchr(prev, ';');
                }
                if (delimiter == NULL) {
                    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                                  "server format error: \"%s\" ", read_line);
                    break;
                }

                upstream_conf = ngx_array_push(&ctx->upstream_conf);
                ngx_memzero(upstream_conf, sizeof(*upstream_conf));
                ngx_sprintf(upstream_conf->sockaddr, "%*s", delimiter - prev, prev);

                /* default value, server attribute */
                upstream_conf->weight = 1;
                upstream_conf->max_fails = 2;
                upstream_conf->fail_timeout = 10;

                upstream_conf->down = 0;
                upstream_conf->backup = 0;

                prev = delimiter;
                delimiter = NULL;

                continue;
            }

            if (ngx_strncmp(prev, "weight=", 7) == 0) {
                prev += 7;
                delimiter = ngx_strchr(prev, ' ');
                if (delimiter == NULL) {
                    delimiter = ngx_strchr(prev, ';');
                }

                if (delimiter == NULL) {
                    continue;
                }

                upstream_conf->weight = ngx_atoi((u_char *)prev, 
                                                 (size_t)(delimiter - prev));

                if (upstream_conf->weight < 0) {
                    upstream_conf->weight = 1;
                }
                prev = delimiter;
                delimiter = NULL;

                continue;
            }

            if (ngx_strncmp(prev, "max_fails=", 10) == 0) {
                prev += 10;
                delimiter = ngx_strchr(prev, ' ');
                if (delimiter == NULL) {
                    delimiter = ngx_strchr(prev, ';');
                }

                if (delimiter == NULL) {
                    continue;
                }

                max_fails = ngx_atoi((u_char *)prev, 
                                     (size_t)(delimiter - prev));

                if (max_fails < 0) {
                    upstream_conf->max_fails = 2;

                } else {
                    upstream_conf->max_fails = max_fails;
                }
                prev = delimiter;
                delimiter = NULL;

                continue;
            }

            if (ngx_strncmp(prev, "fail_timeout=", 13) == 0) {
                prev += 13;
                delimiter = ngx_strchr(prev, ' ');
                if (delimiter == NULL) {
                    delimiter = ngx_strchr(prev, ';');
                }

                if (delimiter == NULL) {
                    continue;
                }

                s.data = (u_char *)prev;
                s.len = delimiter - prev;
                upstream_conf->fail_timeout = ngx_parse_time(&s, 1);

                if (upstream_conf->fail_timeout < 0) {
                    upstream_conf->fail_timeout = 10;
                }
                prev = delimiter;
                delimiter = NULL;

                continue;
            }

            prev++;
        }
    }
    ngx_fclose(fp);

    ngx_http_upsync_add_check((ngx_cycle_t *)ngx_cycle, upsync_server);
    if (ctx->add_upstream.nelts > 0) {

        if (ngx_http_upsync_add_server((ngx_cycle_t *)ngx_cycle, 
                                       upsync_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_dump_file: upstream add server error");
            return NGX_ERROR;
        }
    }

    ngx_http_upsync_del_check((ngx_cycle_t *)ngx_cycle, upsync_server);
    if (ctx->del_upstream.nelts > 0) {

        if (ngx_http_upsync_del_server((ngx_cycle_t *)ngx_cycle, 
                                       upsync_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_dump_file: upstream del server error");
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_add_timers(ngx_cycle_t *cycle)
{
    ngx_msec_t                                   t, tmp;
    ngx_uint_t                                   i;
    ngx_http_upsync_server_t                    *upsync_server;
    ngx_http_upsync_srv_conf_t                  *upscf;

    upsync_server = upsync_ctx->upsync_server;
    if (upsync_server == NULL) {
        return NGX_OK;
    }
    upscf = upsync_server->upscf;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, cycle->log, 0,
                   "upsync_add_timers: shm_name: %V", upsync_server->pc.name);

    srandom(ngx_pid);
    for (i = 0; i < upsync_ctx->upstream_num; i++) {

        upsync_server[i].upsync_ev.handler = ngx_http_upsync_begin_handler;
        upsync_server[i].upsync_ev.log = cycle->log;
        upsync_server[i].upsync_ev.data = &upsync_server[i];
        upsync_server[i].upsync_ev.timer_set = 0;

        upsync_server[i].upsync_timeout_ev.handler =
            ngx_http_upsync_timeout_handler;
        upsync_server[i].upsync_timeout_ev.log = cycle->log;
        upsync_server[i].upsync_timeout_ev.data = &upsync_server[i];
        upsync_server[i].upsync_timeout_ev.timer_set = 0;

        /*
         * We add a random start time here, since we don't want to trigger
         * the check events too close to each other at the beginning.
         */
        tmp = upscf->upsync_interval;
        t = ngx_random() % 1000 + tmp;

        ngx_add_timer(&upsync_server[i].upsync_ev, t);
    }

    return NGX_OK;
}


static void
ngx_http_upsync_begin_handler(ngx_event_t *event)
{
    ngx_http_upsync_ctx_t          *ctx;
    ngx_http_upsync_server_t       *upsync_server;

    if (ngx_http_upsync_need_exit()) {
        return;
    }

    upsync_server = event->data;
    if (upsync_server == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "ngx_http_upsync_begin_handler: upsync_server is null");
        return;
    }

    ctx = &upsync_server->ctx;
    if (ctx->pool != NULL) {
        ngx_destroy_pool(ctx->pool);
    }
    ctx->pool = NULL;

    ngx_memzero(ctx, sizeof(*ctx));

    if (parser != NULL) {
        ngx_free(parser);
    }
    parser = NULL;

    if (upsync_server->upsync_ev.timer_set) {
        ngx_del_timer(&upsync_server->upsync_ev);
    }

    ngx_http_upsync_connect_handler(event);
}


static void
ngx_http_upsync_connect_handler(ngx_event_t *event)
{
    ngx_int_t                                 rc;
    ngx_connection_t                         *c;
    ngx_upsync_conf_t                        *upsync_type_conf;
    ngx_http_upsync_server_t                 *upsync_server;
    ngx_http_upsync_srv_conf_t               *upscf;

    if (ngx_http_upsync_need_exit()) {
        return;
    }

    if (ngx_http_upsync_init_server(event) != NGX_OK) {
        return;
    }

    upsync_server = event->data;
    upscf = upsync_server->upscf;
    upsync_type_conf = upscf->upsync_type_conf;

    ngx_add_timer(&upsync_server->upsync_timeout_ev, upscf->upsync_timeout);

    rc = ngx_event_connect_peer(&upsync_server->pc);
    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        ngx_log_error(NGX_LOG_ERR, event->log, 0,
                      "upsync_connect_handler: cant connect upsync_server: %V ",
                      upsync_server->pc.name);
        return;
    }

    /* NGX_OK or NGX_AGAIN */
    c = upsync_server->pc.connection;
    c->data = upsync_server;
    c->log = upsync_server->pc.log;
    c->sendfile = 0;
    c->read->log = c->log;
    c->write->log = c->log;

    c->write->handler = upsync_type_conf->send_handler;
    c->read->handler = upsync_type_conf->recv_handler;

    /* The kqueue's loop interface needs it. */
    if (rc == NGX_OK) {
        c->write->handler(c->write);
    }
}


static void
ngx_http_upsync_send_handler(ngx_event_t *event)
{
    ssize_t                                   size;
    ngx_connection_t                         *c;
    ngx_upsync_conf_t                        *upsync_type_conf;
    ngx_http_upsync_ctx_t                    *ctx;
    ngx_http_upsync_server_t                 *upsync_server;
    ngx_http_upsync_srv_conf_t               *upscf;

    if (ngx_http_upsync_need_exit()) {
        return;
    }

    c = event->data;
    upsync_server = c->data;
    upscf = upsync_server->upscf;
    upsync_type_conf = upscf->upsync_type_conf;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "upsync_send");

    ctx = &upsync_server->ctx;

    u_char request[ngx_pagesize];
    ngx_memzero(request, ngx_pagesize);

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL) {
        ngx_sprintf(request, "GET %V?recurse&index=%d HTTP/1.0\r\nHost: %V\r\n"
                    "Accept: */*\r\n\r\n", 
                    &upscf->upsync_send, upsync_server->index, 
                    &upscf->conf_server.name);
    }

    ctx->send.pos = request;
    ctx->send.last = ctx->send.pos + ngx_strlen(request);
    while (ctx->send.pos < ctx->send.last) {
        size = c->send(c, ctx->send.pos, ctx->send.last - ctx->send.pos);

#if (NGX_DEBUG)
        {
            ngx_err_t  err;

            err = (size >=0) ? 0 : ngx_socket_errno;
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, err,
                           "upsync_send: send size: %z, total: %z",
                           size, ctx->send.last - ctx->send.pos);
        }
#endif

        if (size > 0) {
            ctx->send.pos += size;

        } else if (size == 0 || size == NGX_AGAIN) {
            return;

        } else {
            c->error = 1;
            goto upsync_send_fail;
        }
    }

    if (ctx->send.pos == ctx->send.last) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, 
                       "upsync_send: send done.");
    }

    return;

upsync_send_fail:
    ngx_log_error(NGX_LOG_ERR, event->log, 0,
                  "upsync_send: send error with upsync_server: %V", 
                  upsync_server->pc.name);

    ngx_http_upsync_clean_event(upsync_server);
}


static void
ngx_http_upsync_recv_handler(ngx_event_t *event)
{
    u_char                                *new_buf;
    ssize_t                                size, n;
    ngx_pool_t                            *pool;
    ngx_connection_t                      *c;
    ngx_upsync_conf_t                     *upsync_type_conf;
    ngx_http_upsync_ctx_t                 *ctx;
    ngx_http_upsync_server_t              *upsync_server;

    if (ngx_http_upsync_need_exit()) {
        return;
    }

    c = event->data;
    upsync_server = c->data;
    upsync_type_conf = upsync_server->upscf->upsync_type_conf;
    ctx = &upsync_server->ctx;

    if (ctx->pool == NULL) {
        pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, ngx_cycle->log);
        if (pool == NULL) {
            ngx_log_error(NGX_LOG_ERR, event->log, 0, 
                          "upsync_recv: recv no enough memory");
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
            goto upsync_recv_fail;
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
                goto upsync_recv_fail;
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
                           "upsync_recv: recv size: %z, upsync_server: %V ",
                           size, upsync_server->pc.name);
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
            goto upsync_recv_fail;
        }
    }

    if (ctx->recv.last != ctx->recv.pos) {

        if (upsync_type_conf->init(upsync_server) == NGX_OK) {
            ngx_http_upsync_process(upsync_server);
        }
    }

    upsync_type_conf->clean(upsync_server);

    return;

upsync_recv_fail:
    ngx_log_error(NGX_LOG_ERR, event->log, 0,
                  "upsync_recv: recv error with upsync_server: %V", 
                  upsync_server->pc.name);

    ngx_http_upsync_clean_event(upsync_server);
}


static ngx_int_t
ngx_http_upsync_parse_init(void *data)
{
    ngx_upsync_conf_t                     *upsync_type_conf;
    ngx_http_upsync_ctx_t                 *ctx;
    ngx_http_upsync_server_t              *upsync_server = data;

    upsync_type_conf = upsync_server->upscf->upsync_type_conf;
    ctx = &upsync_server->ctx;

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL) {
        if (ngx_http_parser_init() == NGX_ERROR) {
            return NGX_ERROR;
        }

        ngx_http_parser_execute(ctx);
        if (ctx->body.pos != ctx->body.last) {
            *(ctx->body.last + 1) = '\0';

        } else {
            return NGX_ERROR;
        }

    } else {
        ctx->body.pos = ctx->recv.pos;
        ctx->body.last = ctx->recv.last;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_dump_server(ngx_http_upsync_server_t *upsync_server)
{
    ngx_buf_t                                       *b=NULL;
    ngx_uint_t                                       i;
    ngx_http_upsync_srv_conf_t                      *upscf = NULL;
    ngx_http_upstream_rr_peers_t                    *peers = NULL;
    ngx_http_upstream_srv_conf_t                    *uscf = NULL;

    uscf = upsync_server->uscf;
    if (uscf->peer.data != NULL) {
        peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    } else {
        ngx_log_error(NGX_LOG_ERR, upsync_server->ctx.pool->log, 0,
                      "upsync_dump_server: no peers");
        return NGX_ERROR;
    }

    if (peers->number == 0) {
        ngx_log_error(NGX_LOG_ERR, upsync_server->ctx.pool->log, 0,
                      "upsync_dump_server: peers number is zero");
        return NGX_ERROR;
    }

    b = ngx_create_temp_buf(upsync_server->ctx.pool, 
                            NGX_PAGE_SIZE * NGX_PAGE_NUMBER);
    if (b == NULL) {
        ngx_log_error(NGX_LOG_ERR, upsync_server->ctx.pool->log, 0,
                      "upsync_dump_server: dump failed %V", &uscf->host);
        return NGX_ERROR;
    }

    for (i = 0; i < peers->number; i++) {
        b->last = ngx_snprintf(b->last,b->end - b->last, 
                               "server %V", &peers->peer[i].name);
        b->last = ngx_snprintf(b->last,b->end - b->last, 
                               " weight=%d", peers->peer[i].weight);
        b->last = ngx_snprintf(b->last,b->end - b->last, 
                               " max_fails=%d", peers->peer[i].max_fails);
        b->last = ngx_snprintf(b->last,b->end - b->last, 
                               " fail_timeout=%ds;\n", peers->peer[i].fail_timeout);
    }

    upscf = upsync_server->upscf;
    upscf->conf_file->fd = ngx_open_file(upscf->upsync_dump_path.data,
                                         NGX_FILE_TRUNCATE,
                                         NGX_FILE_WRONLY,
                                         NGX_FILE_DEFAULT_ACCESS);
    if (upscf->conf_file->fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, upsync_server->ctx.pool->log, 0,
                      "upsync_dump_server: open dump file \"%V\" failed", 
                      &upscf->upsync_dump_path);
        return NGX_ERROR;
    }

    ngx_lseek(upscf->conf_file->fd, 0, SEEK_SET);
    if (ngx_write_fd(upscf->conf_file->fd, b->start, b->last - b->start) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, upsync_server->ctx.pool->log, 0,
                      "upsync_dump_server: write file failed %V", 
                      &upscf->upsync_dump_path);
        ngx_close_file(upscf->conf_file->fd);
        return NGX_ERROR;
    }

    if (ngx_ftruncate(upscf->conf_file->fd, b->last - b->start) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, upsync_server->ctx.pool->log, 0,
                      "upsync_dump_server: truncate file failed %V", 
                      &upscf->upsync_dump_path);
        ngx_close_file(upscf->conf_file->fd);
        return NGX_ERROR;
    }

    ngx_close_file(upscf->conf_file->fd);
    upscf->conf_file->fd = NGX_INVALID_FILE;

    ngx_log_error(NGX_LOG_NOTICE, upsync_server->ctx.pool->log, 0,
                  "upsync_dump_server: dump conf file %V succeed, server numbers is %d", 
                  &upscf->upsync_dump_path, peers->number);

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_init_server(ngx_event_t *event)
{
    ngx_pool_t                              *pool;
    ngx_http_upsync_ctx_t                   *ctx;
    ngx_http_upsync_server_t                *upsync_server;
    ngx_http_upsync_srv_conf_t              *upscf;
    ngx_http_upstream_server_t              *conf_server;

    u_char               *p, *host = NULL;
    size_t                len;
    ngx_str_t            *name;
    struct addrinfo       hints, *res = NULL, *rp = NULL;
    struct sockaddr_in   *sin;

    upsync_server = event->data;
    upscf = upsync_server->upscf;
    conf_server = &upscf->conf_server;

    ctx = &upsync_server->ctx;
    if (ctx->pool == NULL) {

        pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, ngx_cycle->log);
        if (pool == NULL) {
            ngx_log_error(NGX_LOG_ERR, event->log, 0, 
                          "upsync_init_consul: creat pool, no enough memory");
            return NGX_ERROR;
        }
        ctx->pool = pool;
    }

    ngx_memzero(&upsync_server->pc, sizeof(ngx_peer_connection_t));

    upsync_server->pc.get = ngx_event_get_peer;
    upsync_server->pc.log = event->log;
    upsync_server->pc.log_error = NGX_ERROR_ERR;

    upsync_server->pc.cached = 0;
    upsync_server->pc.connection = NULL;

    if (ngx_inet_addr(upscf->upsync_host.data, upscf->upsync_host.len)
            == INADDR_NONE) 
    {

        host = ngx_pcalloc(ctx->pool, upscf->upsync_host.len + 1);
        if (host == NULL) {
            return NGX_ERROR;
        }

        (void) ngx_cpystrn(host, upscf->upsync_host.data, upscf->upsync_host.len + 1);

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
            sin->sin_port = htons((in_port_t) upscf->upsync_port);

            upsync_server->pc.sockaddr = (struct sockaddr *) sin;
            upsync_server->pc.socklen = rp->ai_addrlen;

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

            upsync_server->pc.name = name;

            freeaddrinfo(res);
            return NGX_OK;
        }
    }

valid:

    upsync_server->pc.sockaddr = conf_server->addrs[0].sockaddr;
    upsync_server->pc.socklen = conf_server->addrs[0].socklen;
    upsync_server->pc.name = &conf_server->addrs[0].name;

    if (res != NULL) {
        freeaddrinfo(res);
    }

    return NGX_OK;
}


static void
ngx_http_upsync_event_init(ngx_http_upstream_rr_peers_t *tmp_peers, 
    ngx_http_upsync_server_t *upsync_server, ngx_flag_t flag)
{
    ngx_time_t                                  *tp;
    ngx_delay_event_t                           *delay_event;

    delay_event = ngx_calloc(sizeof(*delay_event), ngx_cycle->log);
    if (delay_event == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_event_init: calloc failed");
        return;
    }

    tp = ngx_timeofday();
    delay_event->start_sec = tp->sec;
    delay_event->start_msec = tp->msec;

    if (flag == NGX_ADD) {
        delay_event->delay_delete_ev.handler = ngx_http_upsync_add_delay_delete;
        delay_event->delay_delete_ev.log = ngx_cycle->log;
        delay_event->delay_delete_ev.data = delay_event;
        delay_event->delay_delete_ev.timer_set = 0;

        ngx_queue_insert_head(&upsync_server->add_ev, &delay_event->queue);
    } else {

        delay_event->delay_delete_ev.handler = ngx_http_upsync_del_delay_delete;
        delay_event->delay_delete_ev.log = ngx_cycle->log;
        delay_event->delay_delete_ev.data = delay_event;
        delay_event->delay_delete_ev.timer_set = 0;

        ngx_queue_insert_head(&upsync_server->delete_ev, &delay_event->queue);
    }

    delay_event->data = tmp_peers;
    ngx_add_timer(&delay_event->delay_delete_ev, NGX_DELAY_DELETE);

    return;
}


static void
ngx_http_upsync_add_delay_delete(ngx_event_t *event)
{
    ngx_uint_t                       i;
    ngx_connection_t                *c;
    ngx_delay_event_t               *delay_event;
    ngx_http_request_t              *r = NULL;
    ngx_http_log_ctx_t              *ctx = NULL;
    ngx_http_upstream_rr_peers_t    *tmp_peers = NULL;

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
ngx_http_upsync_del_delay_delete(ngx_event_t *event)
{
    ngx_uint_t                       i;
    ngx_connection_t                *c;
    ngx_delay_event_t               *delay_event;
    ngx_http_request_t              *r = NULL;
    ngx_http_log_ctx_t              *ctx = NULL;
    ngx_http_upstream_rr_peers_t    *tmp_peers = NULL;

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

            if (tmp_peers->peer[i].down != NGX_MAX_VALUE) {
                continue;
            }

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


static size_t
ngx_strnlen(const char *s, size_t maxlen)
{
    const char *p;

    p = ngx_strchr(s, '\0');
    if (p == NULL) {
        return maxlen;
    }

    return p - s;
}


static size_t
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
    ngx_memzero(state.http_body, NGX_PAGE_SIZE * NGX_PAGE_NUMBER);
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
ngx_http_parser_execute(ngx_http_upsync_ctx_t *ctx)
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


static int
ngx_http_status(http_parser *p, const char *buf, size_t len)
{
    if (p != parser) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "ngx_http_status: parser argument is wrong");
        return NGX_ERROR;
    }

    ngx_memcpy(state.status, buf, len);

    return 0;
}


static int
ngx_http_header_field_cb (http_parser *p, const char *buf, size_t len)
{
    if (p != parser) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "ngx_http_header_field_cb: parser argument is wrong");
        return NGX_ERROR;
    }

    if (state.last_header != FIELD) {
        state.num_headers++;
    }

    ngx_strlncat(state.headers[state.num_headers-1][0],
                 sizeof(state.headers[state.num_headers-1][0]),
                 buf,
                 len);

    state.last_header = FIELD;

    return NGX_OK;
}


static int
ngx_http_header_value_cb (http_parser *p, const char *buf, size_t len)
{
    if (p != parser) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "ngx_http_header_field_cb: parser argument is wrong");
        return NGX_ERROR;
    }

    ngx_strlncat(state.headers[state.num_headers-1][1],
                 sizeof(state.headers[state.num_headers-1][1]),
                 buf,
                 len);

    state.last_header = VALUE;

    return NGX_OK;
}


static int
ngx_http_body(http_parser *p, const char *buf, size_t len)
{
    char *tmp_buf;

    tmp_buf = (char *)state.http_body;

    if (p != parser) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "ngx_http_body: parser argument is wrong");
        return NGX_ERROR;
    }

    ngx_memcpy(tmp_buf, buf, len);

    tmp_buf += len;

    return NGX_OK;
}


static void
ngx_http_upsync_timeout_handler(ngx_event_t *event)
{
    ngx_http_upsync_server_t    *upsync_server;

    if (ngx_http_upsync_need_exit()) {
        return;
    }

    upsync_server = event->data;

    ngx_log_error(NGX_LOG_ERR, event->log, 0,
                  "upsync_timeout: time out with upsync_server: %V ", 
                  upsync_server->pc.name);

    ngx_http_upsync_clean_event(upsync_server);
}


static void
ngx_http_upsync_clean_event(void *data)
{
    ngx_msec_t                          t, tmp;
    ngx_pool_t                         *pool;
    ngx_connection_t                   *c;
    ngx_upsync_conf_t                  *upsync_type_conf;
    ngx_http_upsync_ctx_t              *ctx;
    ngx_http_upsync_server_t           *upsync_server = data;
    ngx_http_upsync_srv_conf_t         *upscf;

    upscf = upsync_server->upscf;
    upsync_type_conf = upscf->upsync_type_conf;

    ctx = &upsync_server->ctx;
    pool = ctx->pool;

    c = upsync_server->pc.connection;

    if (c) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "upsync_clean_event: clean event: fd: %d", c->fd);

        ngx_close_connection(c);
        upsync_server->pc.connection = NULL;
    }

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL) {

        if (parser != NULL) {
            ngx_free(parser);
        }
        parser = NULL;
    }

    if (pool != NULL) {
        ngx_destroy_pool(pool);
    }
    ctx->pool = NULL;

    if (!upsync_server->upsync_ev.timer_set) {
        tmp = upscf->upsync_interval;
        t = ngx_random() % 1000 + tmp;
        ngx_add_timer(&upsync_server->upsync_ev, t);
    }

    if (upsync_server->upsync_timeout_ev.timer_set) {
        ngx_del_timer(&upsync_server->upsync_timeout_ev);
    }

    return;
}


static ngx_int_t
ngx_http_upsync_need_exit()
{
    if (ngx_terminate || ngx_exiting || ngx_quit) {
        ngx_http_upsync_clear_all_events();

        return 1;
    }

    return NGX_OK;
}


static void
ngx_http_upsync_clear_all_events()
{
    ngx_uint_t                          i;
    ngx_connection_t                   *c;
    ngx_upsync_conf_t                  *upsync_type_conf;
    ngx_http_upsync_server_t           *upsync_server;

    static ngx_flag_t                   has_cleared = 0;

    if (has_cleared || upsync_ctx == NULL) {
        return;
    }

    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "[WARN]:"
                  "upsync_clear_all_events: on %P ", ngx_pid);

    has_cleared = 1;

    upsync_server = upsync_ctx->upsync_server;
    upsync_type_conf = upsync_server->upscf->upsync_type_conf;

    for (i = 0; i < upsync_ctx->upstream_num; i++) {

        if (upsync_server[i].upsync_ev.timer_set) {
            ngx_del_timer(&upsync_server[i].upsync_ev);
        }

        if (upsync_server[i].upsync_timeout_ev.timer_set) {
            c = upsync_server[i].pc.connection;
            if (c) {
                ngx_close_connection(c);
                upsync_server->pc.connection = NULL;
            }
            ngx_del_timer(&upsync_server[i].upsync_timeout_ev);
        }
    }

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL) {

        if (parser != NULL) {
            ngx_free(parser);
        }
        parser = NULL;
    }

    return;
}


static ngx_int_t
ngx_http_upsync_get_upstream(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server, char **conf_value)
{
    ngx_http_conf_client *client = ngx_http_create_client(cycle, upsync_server);

    if (client == NULL) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_get_upstream: http client create error");
        return NGX_ERROR;
    }

    ngx_int_t status = ngx_http_client_conn(client);
    if (status != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_get_upstream: http client conn error");

        ngx_http_client_destroy(client);
        return NGX_ERROR;
    }

    char *response = NULL;

    ngx_http_client_send(client, upsync_server);

    if (ngx_http_client_recv(client, &response, 0) <= 0) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_get_upstream: http client recv fail");

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
ngx_http_create_client(ngx_cycle_t *cycle, ngx_http_upsync_server_t *upsync_server)
{
    ngx_http_conf_client                         *client = NULL;
    ngx_http_upstream_server_t                   *conf_server;
    ngx_http_upsync_srv_conf_t                   *upscf;

    upscf = upsync_server->upscf;
    conf_server = &upscf->conf_server;

    client = ngx_calloc(sizeof(ngx_http_conf_client), cycle->log);
    if (client == NULL) {
        return NULL;
    }

    client->sd = -1;
    client->connected = 0;
    client->addr = *(struct sockaddr_in *)conf_server->addrs[0].sockaddr;

    if((client->sd = socket(AF_INET,SOCK_STREAM, 0)) == NGX_ERROR) {
        ngx_free(client);
        client = NULL;

        return NULL;
    }

    struct timeval tv_timeout;
    tv_timeout.tv_sec = NGX_HTTP_SOCKET_TIMEOUT;
    tv_timeout.tv_usec = 0;

    if (setsockopt(client->sd, SOL_SOCKET, SO_SNDTIMEO, (void *) &tv_timeout, 
                   sizeof(struct timeval)) < 0) 
    {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "ngx_http_create_client: setsockopt SO_SNDTIMEO error");
        ngx_http_client_destroy(client);
        return NULL;
    }

    if (setsockopt(client->sd, SOL_SOCKET, SO_RCVTIMEO, (void *) &tv_timeout, 
                   sizeof(struct timeval)) < 0) 
    {
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
    if (connect(client->sd, (struct sockaddr *)&(client->addr), 
                sizeof(struct sockaddr)) == NGX_ERROR) {
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
    ngx_http_upsync_server_t *upsync_server)
{
    size_t       size = 0;
    ngx_int_t    tmp_send = 0;
    ngx_uint_t   send_num = 0;

    ngx_upsync_conf_t           *upsync_type_conf;
    ngx_http_upsync_srv_conf_t  *upscf;

    upscf = upsync_server->upscf;
    upsync_type_conf = upscf->upsync_type_conf;

    u_char request[ngx_pagesize];
    ngx_memzero(request, ngx_pagesize);

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL) {
        ngx_sprintf(request, "GET %V?recurse&index=%d HTTP/1.0\r\nHost: %V\r\n"
                    "Accept: */*\r\n\r\n", 
                    &upscf->upsync_send, upsync_server->index, 
                    &upscf->conf_server.name);
    }

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
ngx_http_upsync_set(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_upsync_show;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_upsync_show(ngx_http_request_t *r)
{
    ngx_buf_t                             *b;
    ngx_int_t                              rc, ret;
    ngx_str_t                             *host;
    ngx_uint_t                             i;
    ngx_chain_t                            out;
    ngx_http_upstream_rr_peers_t          *peers = NULL;
    ngx_http_upstream_srv_conf_t         **uscfp = NULL, *uscf = NULL;
    ngx_http_upstream_main_conf_t         *umcf;

    umcf = ngx_http_cycle_get_module_main_conf(ngx_cycle, 
                                               ngx_http_upstream_module);

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

    b = ngx_create_temp_buf(r->pool, NGX_PAGE_SIZE * NGX_PAGE_NUMBER);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    out.buf = b;
    out.next = NULL;

    host = &r->args;
    if (host->len == 0 || host->data == NULL) {

        b->last = ngx_snprintf(b->last,b->end - b->last, 
                               "Please input specific upstream name");
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

        b->last = ngx_snprintf(b->last,b->end - b->last, 
                               "The upstream name you input is not exited,"
                               "Please check and input again");
        goto end;
    }

    if (uscf->peer.data != NULL) {
        peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
    }

    b->last = ngx_snprintf(b->last,b->end - b->last, 
                           "Upstream name: %V;", host);
    b->last = ngx_snprintf(b->last,b->end - b->last, 
                           "Backend server counts: %d\n", peers->number);

    for (i = 0; i < peers->number; i++) {
        b->last = ngx_snprintf(b->last,b->end - b->last, 
                               "        server %V", &peers->peer[i].name);
        b->last = ngx_snprintf(b->last,b->end - b->last, 
                               " weight=%d", peers->peer[i].weight);
        b->last = ngx_snprintf(b->last,b->end - b->last, 
                               " max_fails=%d", peers->peer[i].max_fails);
        b->last = ngx_snprintf(b->last,b->end - b->last, 
                               " fail_timeout=%d;\n", peers->peer[i].fail_timeout);
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
