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
#define NGX_HTTP_UPSYNC_CONSUL_SERVICES      0x0002
#define NGX_HTTP_UPSYNC_CONSUL_HEALTH        0x0003
#define NGX_HTTP_UPSYNC_ETCD                 0x0004


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

    ngx_int_t                        upsync_lb;
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

    uint64_t                                 index;
    uint64_t                                 update_generation;

    ngx_event_t                              upsync_ev;
    ngx_event_t                              upsync_timeout_ev;

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


static ngx_upsync_conf_t *ngx_http_upsync_get_type_conf(ngx_str_t *str);
static char *ngx_http_upsync_set_lb(ngx_conf_t *cf, ngx_command_t *cmd, 
    void *conf);
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

static void ngx_http_upsync_begin_handler(ngx_event_t *event);
static void ngx_http_upsync_connect_handler(ngx_event_t *event);
static void ngx_http_upsync_recv_handler(ngx_event_t *event);
static void ngx_http_upsync_send_handler(ngx_event_t *event);
static void ngx_http_upsync_recv_empty_handler(ngx_event_t *event);
static void ngx_http_upsync_send_empty_handler(ngx_event_t *event);
static void ngx_http_upsync_timeout_handler(ngx_event_t *event);
static void ngx_http_upsync_clean_event(void *upsync_server);
static ngx_int_t ngx_http_upsync_etcd_parse_init(void *upsync_server);
static ngx_int_t ngx_http_upsync_consul_parse_init(void *upsync_server);
static ngx_int_t ngx_http_upsync_dump_server(
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_upsync_init_server(ngx_event_t *event);

static ngx_int_t ngx_http_upsync_add_peers(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_upsync_del_peers(ngx_cycle_t *cycle,
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_upsync_replace_peers(ngx_cycle_t *cycle,
    ngx_http_upsync_server_t *upsync_server);
static void ngx_http_upsync_update_peer(ngx_http_upstream_rr_peers_t *peers,
    ngx_http_upstream_rr_peer_t *peer,
    ngx_http_upsync_conf_t *upstream_conf,
    ngx_uint_t *updated);
static void ngx_http_upsync_diff_filter(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server,
    ngx_uint_t *diff);

static void ngx_http_upsync_event_init(ngx_http_upstream_rr_peer_t *peer, 
    ngx_http_upsync_server_t *upsync_server);

static ngx_int_t ngx_http_parser_init();

static int ngx_http_status(http_parser *p, const char *buf, size_t len);
static int ngx_http_header_field_cb(http_parser *p, const char *buf, 
    size_t len);
static int ngx_http_header_value_cb(http_parser *p, const char *buf, 
    size_t len);
static int ngx_http_body(http_parser *p, const char *buf, size_t len);

static ngx_int_t ngx_http_upsync_check_index(
    ngx_http_upsync_server_t *upsync_server);
static ngx_int_t ngx_http_upsync_consul_parse_json(void *upsync_server);
static ngx_int_t ngx_http_upsync_consul_services_parse_json(
    void *upsync_server);
static ngx_int_t ngx_http_upsync_consul_health_parse_json(
    void *upsync_server);
static ngx_int_t ngx_http_upsync_etcd_parse_json(void *upsync_server);
static ngx_int_t ngx_http_upsync_check_key(u_char *key, ngx_str_t host);
static void *ngx_http_upsync_servers(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server, ngx_flag_t flag);
static void *ngx_http_upsync_addrs(ngx_pool_t *pool, u_char *sockaddr);

static void ngx_http_upsync_del_delay_delete(ngx_event_t *event);

static ngx_int_t ngx_http_upsync_need_exit();
static void ngx_http_upsync_clear_all_events(ngx_cycle_t *cycle);

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

    {  ngx_string("upsync_lb"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_http_upsync_set_lb,
        0,
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
    ngx_http_upsync_clear_all_events,           /* exit process */
    NULL,                                       /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_upsync_conf_t  ngx_upsync_types[] = {

    { ngx_string("consul"),
      NGX_HTTP_UPSYNC_CONSUL,
      ngx_http_upsync_send_handler,
      ngx_http_upsync_recv_handler,
      ngx_http_upsync_consul_parse_init,
      ngx_http_upsync_consul_parse_json,
      ngx_http_upsync_clean_event },

    { ngx_string("consul_services"),
      NGX_HTTP_UPSYNC_CONSUL_SERVICES,
      ngx_http_upsync_send_handler,
      ngx_http_upsync_recv_handler,
      ngx_http_upsync_consul_parse_init,
      ngx_http_upsync_consul_services_parse_json,
      ngx_http_upsync_clean_event },

    { ngx_string("consul_health"),
      NGX_HTTP_UPSYNC_CONSUL_HEALTH,
      ngx_http_upsync_send_handler,
      ngx_http_upsync_recv_handler,
      ngx_http_upsync_consul_parse_init,
      ngx_http_upsync_consul_health_parse_json,
      ngx_http_upsync_clean_event },

    { ngx_string("etcd"),
      NGX_HTTP_UPSYNC_ETCD,
      ngx_http_upsync_send_handler,
      ngx_http_upsync_recv_handler,
      ngx_http_upsync_etcd_parse_init,
      ngx_http_upsync_etcd_parse_json,
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

            upscf->upsync_type_conf = ngx_http_upsync_get_type_conf(&s);
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
ngx_http_upsync_get_type_conf(ngx_str_t *str)
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
ngx_http_upsync_set_lb(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                         *value, *str;
    ngx_http_upsync_srv_conf_t        *upscf;

    upscf = ngx_http_conf_get_module_srv_conf(cf,
                                              ngx_http_upsync_module);
    value = cf->args->elts;

    str = &value[1];
    if (str->len == NGX_CONF_UNSET_SIZE) {
        upscf->upsync_lb = NGX_HTTP_LB_DEFAULT;

        return NGX_CONF_OK; 
    }

    switch(str->len) {
        case 7:
            if (ngx_memcmp((char *)str->data, "ip_hash", 7) == 0) {
                upscf->upsync_lb = NGX_HTTP_LB_IP_HASH;

                return NGX_CONF_OK;
            }

            break;

        case 10:
            if (ngx_memcmp((char *)str->data, "roundrobin", 10) == 0) {
                upscf->upsync_lb = NGX_HTTP_LB_ROUNDROBIN;

                return NGX_CONF_OK;
            }

            if (ngx_memcmp((char *)str->data, "least_conn", 10) == 0) {
                upscf->upsync_lb = NGX_HTTP_LB_LEAST_CONN;

                return NGX_CONF_OK;
            }

            break;

        case 11:
            if (ngx_memcmp((char *)str->data, "hash_modula", 11) == 0) {
                upscf->upsync_lb = NGX_HTTP_LB_HASH_MODULA;

                return NGX_CONF_OK;
            }

            if (ngx_memcmp((char *)str->data, "hash_ketama", 11) == 0) {
                upscf->upsync_lb = NGX_HTTP_LB_HASH_KETAMA;

                return NGX_CONF_OK;
            }

            break;
    }

    return NGX_CONF_OK;
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
    ngx_uint_t                   diff = 0;
    ngx_upsync_conf_t           *upsync_type_conf;
    ngx_http_upsync_ctx_t       *ctx;

    ctx = &upsync_server->ctx;
    upsync_type_conf = upsync_server->upscf->upsync_type_conf;

    if (ngx_http_upsync_need_exit()) {
        return;
    }

    if (ngx_http_upsync_check_index(upsync_server) == NGX_ERROR) {
        return;
    }

    if (upsync_type_conf->parse(upsync_server) == NGX_ERROR) {
        if (upsync_server->index != 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_process: parse json error");
        }
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG, ngx_cycle->log, 0,
                   "upsync_process: parse json success");
                   
    ngx_http_upsync_diff_filter((ngx_cycle_t *)ngx_cycle, upsync_server, &diff);
    
    if (ctx->add_upstream.nelts > 0) {

        if (upsync_server->update_generation == 0) {
            if (ngx_http_upsync_replace_peers((ngx_cycle_t *)ngx_cycle, 
                                               upsync_server) != NGX_OK) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                           "upsync_process: upstream add/replace peers failed");
                return;
            }
            upsync_server->update_generation++;
        }

        if (ngx_http_upsync_add_peers((ngx_cycle_t *)ngx_cycle, 
                                       upsync_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_process: upstream add peers failed");
            return;
        }
    }

    if (ctx->del_upstream.nelts > 0) {

        if (upsync_server->update_generation == 0) {
            if (ngx_http_upsync_replace_peers((ngx_cycle_t *)ngx_cycle, 
                                               upsync_server) != NGX_OK) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                           "upsync_process: upstream del/replace peers failed");
                return;
            }
            upsync_server->update_generation++;
        }

        if (ngx_http_upsync_del_peers((ngx_cycle_t *)ngx_cycle, 
                                       upsync_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_process: upstream del peers failed");
            return;
        }
    }
    
    if (diff) {
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
    ngx_uint_t                   i;
    uint64_t                     index = 0;
    ngx_upsync_conf_t           *upsync_type_conf;

    upsync_type_conf = upsync_server->upscf->upsync_type_conf;

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL_SERVICES
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL_HEALTH)
    {
        for (i = 0; i < state.num_headers; i++) {

            if (ngx_memcmp(state.headers[i][0], NGX_INDEX_HEADER, 
                           NGX_INDEX_HEADER_LEN) == 0) {
                p = ngx_strchr(state.headers[i][1], '\r');
                *p = '\0';
                index = ngx_strtoull((char *)state.headers[i][1], 
                                     (char **)NULL, 10);
                break;
            }
        }

        if (index == upsync_server->index) {
            ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                          "upsync_check_index: upstream index has not changed: %V",
                          &upsync_server->upscf->upsync_dump_path);
            return NGX_ERROR;

        } else {
            upsync_server->index = index;
        }
    }

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_ETCD) {
        for (i = 0; i < state.num_headers; i++) {

            if (ngx_memcmp(state.headers[i][0], NGX_INDEX_ETCD_HEADER, 
                           NGX_INDEX_ETCD_HEADER_LEN) == 0) {
                p = ngx_strchr(state.headers[i][1], '\r');
                *p = '\0';
                index = ngx_strtoull((char *)state.headers[i][1], 
                                     (char **)NULL, 10);
                break;
            }
        }

        upsync_server->index = index + 1;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_add_peers(ngx_cycle_t *cycle,
    ngx_http_upsync_server_t *upsync_server)
{
    ngx_uint_t                     i=0, n=0, w=0, len=0;
    ngx_array_t                   *servers;
    ngx_http_upstream_server_t    *server = NULL;
    ngx_http_upstream_rr_peer_t   *peer = NULL;
    ngx_http_upstream_rr_peers_t  *peers = NULL;
    ngx_http_upstream_srv_conf_t  *uscf;

    u_char *namep = NULL;
    struct sockaddr *saddr = NULL;
    len = sizeof(struct sockaddr);

    uscf = upsync_server->uscf;

    if (ngx_http_upsync_need_exit()) {
        return NGX_OK;
    }

    servers = ngx_http_upsync_servers(cycle, upsync_server, NGX_ADD);
    if (servers == NULL) {
        return NGX_ERROR;
    }

    if (servers->nelts < 1) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_add_peers: no servers to add \"%V\"", &uscf->host);
        return NGX_ERROR;
    }

    if (uscf->peer.data == NULL) {
        return NGX_ERROR;
    }
    peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    if (peers && servers->nelts >= 1) {
        n = peers->number + servers->nelts;

        for (i = 0; i < servers->nelts; i++) {

            server = (ngx_http_upstream_server_t *)servers->elts + i;
            //if (server->backup) {
            //    continue;
            //}
            //
            // FIXME: until backup is fully implemented this causes crashes
            //        on startup with nodes set backup=1. Let them in for now

            peer = ngx_calloc(sizeof(ngx_http_upstream_rr_peer_t), 
                               cycle->log);
            if (peer == NULL) {
                goto invalid;
            }

            if ((saddr = ngx_calloc(len, cycle->log)) == NULL) {
                goto invalid;
            }
            ngx_memcpy(saddr, server->addrs->sockaddr, len);
            peer->sockaddr = saddr;

            if ((namep = ngx_calloc(server->addrs->name.len,
                                    cycle->log)) == NULL) {
                goto invalid;
            }
            ngx_memcpy(namep, server->addrs->name.data,
                       server->addrs->name.len);
            peer->name.data = namep;
            peer->server.data = namep;

            peer->server.len = server->addrs->name.len;
            peer->socklen = server->addrs->socklen;
            peer->name.len = server->addrs->name.len;
            peer->max_fails = server->max_fails;
            peer->fail_timeout = server->fail_timeout;
            peer->down = server->down;
            peer->weight = server->weight;
            peer->effective_weight = server->weight;
            peer->current_weight = 0;

            peer->conns = 0;

            peer->next = peers->peer;
            peers->peer = peer;

#if (NGX_HTTP_UPSTREAM_CHECK) 
            ngx_uint_t index;
            ngx_addr_t *addrs;

            //TODO: it's a little trick, a issue with upstream_check_module
            // add/del interface, not rely on addrs of check_peers;
            addrs = ngx_pcalloc(ngx_cycle->pool, sizeof(ngx_addr_t));
            if (addrs == NULL) {
                goto invalid;
            }
            addrs->name.data = peer->name.data;
            addrs->name.len = peer->name.len;
            addrs->sockaddr = peer->sockaddr;
            addrs->socklen = peer->socklen;

            index = ngx_http_upstream_check_add_dynamic_peer(cycle->pool, 
                                                             uscf, addrs);
            peer->check_index = index;
#endif
            w += server->weight;
        }
        w += peers->total_weight;

        peers->single = (n == 1);
        peers->number = n;
        peers->weighted = (w != n);
        peers->total_weight = w;

        if (upsync_server->upscf->upsync_lb == NGX_HTTP_LB_HASH_KETAMA) {
            ngx_http_upsync_chash_init(uscf, peers);
        }
    }

    return NGX_OK;

invalid:
    ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                  "upsync_add_peers: calloc peer failed \"%V\"", 
                  &uscf->host);

    if (peer != NULL) {
        if (peer->sockaddr != NULL) {
            ngx_free(peer->sockaddr);
        }
 
        ngx_free(peer);
        peer = NULL;
    }

    return NGX_ERROR;
}


static void
ngx_http_upsync_update_peer(ngx_http_upstream_rr_peers_t *peers,
    ngx_http_upstream_rr_peer_t *peer,
    ngx_http_upsync_conf_t *upstream_conf,
    ngx_uint_t *updated)
{
    ngx_uint_t  w = peers->total_weight, pw = 0;

    *updated = 0;

    if (peer->max_fails == upstream_conf->max_fails &&
        peer->fail_timeout == upstream_conf->fail_timeout &&
        peer->down == upstream_conf->down &&
        peer->weight == upstream_conf->weight) {
        return;
    }

    pw = peer->weight;
    peer->max_fails = upstream_conf->max_fails;
    peer->fail_timeout = upstream_conf->fail_timeout;
    peer->down = upstream_conf->down;
    peer->weight = upstream_conf->weight;
    peer->effective_weight = upstream_conf->weight;
    peer->current_weight = 0;

    w = w + upstream_conf->weight - pw;

    peers->weighted = (w != peers->number);
    peers->total_weight = w;

    *updated = 1;

    return;
}


static void
ngx_http_upsync_diff_filter(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server,
    ngx_uint_t *diff)
{
    ngx_uint_t                          i, j, len, updated;
    ngx_uint_t                         *flags = NULL;
    ngx_array_t                         flag_array;
    ngx_http_upsync_ctx_t              *ctx;
    ngx_http_upsync_conf_t             *upstream_conf;
    ngx_http_upsync_conf_t             *add_upstream, *del_upstream;
    ngx_http_upstream_rr_peer_t        *peer = NULL;
    ngx_http_upstream_rr_peers_t       *peers = NULL;
    ngx_http_upstream_srv_conf_t       *uscf;

    *diff = 0;
    ctx = &upsync_server->ctx;

    if (ngx_http_upsync_need_exit()) {
        return;
    }

    if (ngx_array_init(&ctx->add_upstream, ctx->pool, 16,
                       sizeof(*add_upstream)) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_diff_filter_add: alloc error");
        return;
    }

    if (ngx_array_init(&ctx->del_upstream, ctx->pool, 16,
                       sizeof(*del_upstream)) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_diff_filter_del: alloc error");
        return;
    }

    uscf = upsync_server->uscf;
    if (uscf->peer.data == NULL) {
        return;
    }
    
    peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
    if (peers->number != 0) {
        if (ngx_array_init(&flag_array, ctx->pool, peers->number,
                       sizeof(*flags)) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                          "upsync_diff_filter: alloc error");
            return;
        }

        ngx_memzero(flag_array.elts, sizeof(ngx_uint_t) * flag_array.nalloc);
        flags = (ngx_uint_t*)flag_array.elts;
    }

    len = ctx->upstream_conf.nelts;
    for (i = 0; i < len; i++) {
        upstream_conf = (ngx_http_upsync_conf_t *)ctx->upstream_conf.elts + i;

        for (peer = peers->peer, j = 0; peer; peer = peer->next, j++) {
            if (*(flags + j) == 1) {
                continue;
            }

            if (ngx_memn2cmp(peer->name.data, upstream_conf->sockaddr,
                             peer->name.len,
                             ngx_strlen(upstream_conf->sockaddr)) == 0) {
                // update peer
                ngx_http_upsync_update_peer(peers, peer, upstream_conf, &updated);
                *diff |= updated;

                // set flag, not to be deleted
                *(flags + j) = 1;

                break;
            }
        }

        // add_upstream
        if (j == peers->number) {
            add_upstream = ngx_array_push(&ctx->add_upstream);
            ngx_memcpy(add_upstream, upstream_conf, sizeof(*upstream_conf));
        }
    }

    // del_upstream
    for (peer = peers->peer, j = 0; peer; peer = peer->next, j++) {
        if (*(flags + j) == 1) {
            continue;
        }

        del_upstream = ngx_array_push(&ctx->del_upstream);
        ngx_memzero(del_upstream, sizeof(*del_upstream));
        ngx_memcpy(&del_upstream->sockaddr, peer->name.data, peer->name.len);
    }

    *diff |= (ctx->add_upstream.nelts > 0);
    *diff |= (ctx->del_upstream.nelts > 0);

    return;
}

static ngx_int_t
ngx_http_upsync_del_peers(ngx_cycle_t *cycle,
    ngx_http_upsync_server_t *upsync_server)
{
    ngx_uint_t                     i, n=0, w=0, len;
    ngx_array_t                   *servers;
    ngx_http_upstream_server_t    *server = NULL;
    ngx_http_upstream_rr_peer_t   *peer = NULL, *pre_peer = NULL;
    ngx_http_upstream_rr_peer_t   *del_peer = NULL, *tmp_del_peer = NULL;
    ngx_http_upstream_rr_peers_t  *peers = NULL;
    ngx_http_upstream_srv_conf_t  *uscf;

    len = sizeof(struct sockaddr);
    uscf = upsync_server->uscf;

    if (ngx_http_upsync_need_exit()) {
        return NGX_OK;
    }

    servers = ngx_http_upsync_servers(cycle, upsync_server, NGX_DEL);
    if (servers == NULL) {
        return NGX_ERROR;
    }

    if (servers->nelts < 1) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_del_peers: no servers to delete \"%V\"", &uscf->host);
        return NGX_ERROR;
    }

    if (uscf->peer.data == NULL) {
        return NGX_ERROR;
    }
    
    peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    if (peers->number <= servers->nelts) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_del_peer: upstream \"%V\" cannot delete all peers", 
                      &uscf->host);
        return NGX_ERROR;
    }

    n = peers->number - servers->nelts;
    w = peers->total_weight;

    pre_peer = peers->peer;
    for (peer = peers->peer; peer; peer = peer->next) {
        for (i = 0; i < servers->nelts; i++) {

            server = (ngx_http_upstream_server_t *)servers->elts + i;
            if (ngx_memn2cmp((u_char *) peer->sockaddr,
                             (u_char *) server->addrs->sockaddr,
                             len, len) == 0)
            {

#if (NGX_HTTP_UPSTREAM_CHECK) 
                ngx_http_upstream_check_delete_dynamic_peer(
                                                    peers->name, server->addrs);
#endif
                if (del_peer == NULL) {
                    del_peer = peer;
                    tmp_del_peer = peer;

                } else {
                    tmp_del_peer->next = peer;
                    tmp_del_peer = peer;
                }

                if (pre_peer == peer) {

                    peers->peer = peer->next;
                    pre_peer = peer->next;

                } else {
                    pre_peer->next = peer->next;
                }

                w -= peer->weight;
                break;
            }
        }

        if (i == servers->nelts) {
            pre_peer = peer;
        }
    }

    if (tmp_del_peer) {
        tmp_del_peer->next = NULL;
    }

    peers->single = (n == 1);
    peers->number = n;
    peers->weighted = (w != n);
    peers->total_weight = w;

    if (upsync_server->upscf->upsync_lb == NGX_HTTP_LB_HASH_KETAMA) {
        ngx_http_upsync_del_chash_peer(uscf);
    }

    ngx_http_upsync_event_init(del_peer, upsync_server);

    return NGX_OK;
}

static ngx_int_t
ngx_http_upsync_replace_peers(ngx_cycle_t *cycle,
    ngx_http_upsync_server_t *upsync_server)
{
    ngx_uint_t                      i, len;
    ngx_http_upstream_rr_peer_t    *peer = NULL, *tmp_peer;
    ngx_http_upstream_rr_peers_t   *peers = NULL;
    ngx_http_upstream_srv_conf_t   *uscf;

    uscf = upsync_server->uscf;

    u_char *namep = NULL;
    struct sockaddr *saddr = NULL;
    len = sizeof(struct sockaddr);


    if (uscf->peer.data == NULL) {
        return NGX_ERROR;
    }
    
    peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    tmp_peer = peers->peer;
    if (peers) {

        for (i = 0; i < peers->number; i++) {
            peer = ngx_calloc(sizeof(ngx_http_upstream_rr_peer_t), 
                                     cycle->log);
            if (peer == NULL) {
                goto invalid;
            }

            if ((saddr = ngx_calloc(len, cycle->log)) == NULL) {
                goto invalid;
            }
            ngx_memcpy(saddr, tmp_peer[i].sockaddr, len);
            peer->sockaddr = saddr;

            peer->socklen = tmp_peer[i].socklen;
            peer->name.len = tmp_peer[i].name.len;
            peer->server.len = tmp_peer[i].name.len;

            if ((namep = ngx_calloc(tmp_peer[i].name.len,
                                    cycle->log)) == NULL) {
                goto invalid;
            }
            ngx_memcpy(namep, tmp_peer[i].name.data, tmp_peer[i].name.len);
            peer->name.data = namep;
            peer->server.data = namep;

            peer->max_fails = tmp_peer[i].max_fails;
            peer->fail_timeout = tmp_peer[i].fail_timeout;
            peer->down = tmp_peer[i].down;
            peer->weight = tmp_peer[i].weight;
            peer->effective_weight = tmp_peer[i].effective_weight;
            peer->current_weight = tmp_peer[i].current_weight;

            peer->conns = 0;

#if (NGX_HTTP_UPSTREAM_CHECK) 
            peer->check_index = tmp_peer[i].check_index;
#endif
            peer->next = peers->peer;
            peers->peer = peer;

            if(i == 0) {
                peer->next = NULL;
            }
        }

        if (upsync_server->upscf->upsync_lb == NGX_HTTP_LB_HASH_KETAMA) {
            ngx_http_upsync_chash_init(uscf, NULL);
        }

        //ngx_pfree(cycle->pool, tmp_peer);  not free for causing address invalid.
    }

    return NGX_OK;

invalid:
    ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                  "upsync_init_peers: copy failed \"%V\"", &uscf->host);

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_upsync_consul_parse_json(void *data)
{
    u_char                         *p;
    ngx_buf_t                      *buf;
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
        cJSON_Delete(root);
        return NGX_ERROR;
    }

    cJSON *server_next;
    for (server_next = root->child; server_next != NULL; 
         server_next = server_next->next) 
    {
        cJSON *temp1 = cJSON_GetObjectItem(server_next, "Key");
        if (temp1 != NULL && temp1->valuestring != NULL) {
            if (ngx_http_upsync_check_key((u_char *)temp1->valuestring,
                                          upsync_server->host) != NGX_OK) {
                continue;
            }

            p = (u_char *)ngx_strrchr(temp1->valuestring, '/');
            upstream_conf = ngx_array_push(&ctx->upstream_conf);
            ngx_memzero(upstream_conf, sizeof(*upstream_conf));
            ngx_sprintf(upstream_conf->sockaddr, "%*s", ngx_strlen(p + 1), p + 1);
        }
        temp1 = NULL;

        if (upstream_conf == NULL) {
            continue;
        }

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
                              "upsync_parse_json: parse \'%s\' failed", p);
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
                          ", setting default value 1");
            upstream_conf->weight = 1;
        }

        if (max_fails < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"max_fails\" value is invalid"
                          ", setting default value 2");
        } else {
            upstream_conf->max_fails = (ngx_uint_t)max_fails;
        }

        if (upstream_conf->fail_timeout < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"fail_timeout\" value is invalid"
                          ", setting default value 10");
            upstream_conf->fail_timeout = 10;
        }

        if (down != 1 && down != 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"down\" value is invalid"
                          ", setting default value 0");
        } else {
            upstream_conf->down = (ngx_uint_t)down;
        }

        if (backup != 1 && backup != 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"backup\" value is invalid"
                          ", setting default value 0");
        } else {
            upstream_conf->backup = (ngx_uint_t)backup;
        }

        max_fails=2, backup=0, down=0;
    }
    cJSON_Delete(root);

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_consul_services_parse_json(void *data)
{
    ngx_buf_t                      *buf;
    ngx_http_upsync_ctx_t          *ctx;
    ngx_http_upsync_conf_t         *upstream_conf = NULL;
    ngx_http_upsync_server_t       *upsync_server = data;
    ngx_int_t                       attr_value;

    ctx = &upsync_server->ctx;
    buf = &ctx->body;

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
        cJSON_Delete(root);
        return NGX_ERROR;
    }

    cJSON *server_next;
    for (server_next = root->child; server_next != NULL; 
         server_next = server_next->next) 
    {
        cJSON *addr, *port, *tags, *tag_next;
        size_t addr_len, port_len;
        u_char port_buf[8];

        addr = cJSON_GetObjectItem(server_next, "ServiceAddress");
        if (addr == NULL || addr->valuestring == NULL
            || addr->valuestring[0] == '\0')
        {
            addr = cJSON_GetObjectItem(server_next, "Address");
            if (addr == NULL || addr->valuestring == NULL) {
                continue;
            }
        }

        port = cJSON_GetObjectItem(server_next, "ServicePort");
        if (port == NULL || port->valueint < 1 || port->valueint > 65535) {
            continue;
        }
        ngx_memzero(port_buf, 8);
        ngx_sprintf(port_buf, "%d", port->valueint);

        addr_len = ngx_strlen(addr->valuestring);
        port_len = ngx_strlen(port_buf);

        if (addr_len + port_len + 2 > NGX_SOCKADDRLEN) {
            continue;
        }

        upstream_conf = ngx_array_push(&ctx->upstream_conf);
        if (upstream_conf == NULL) {
            cJSON_Delete(root);
            return NGX_ERROR;
        }
        ngx_memzero(upstream_conf, sizeof(*upstream_conf));

        ngx_memcpy(upstream_conf->sockaddr, addr->valuestring, addr_len);
        ngx_memcpy(upstream_conf->sockaddr + addr_len, ":", 1);
        ngx_memcpy(upstream_conf->sockaddr + addr_len + 1, port_buf, port_len);

        /* default value, server attribute */
        upstream_conf->weight = 1;
        upstream_conf->max_fails = 2;
        upstream_conf->fail_timeout = 10;

        upstream_conf->down = 0;
        upstream_conf->backup = 0;

        tags = cJSON_GetObjectItem(server_next, "ServiceTags");
        if (tags == NULL) {
            continue;
        }

        for (tag_next = tags->child; tag_next != NULL; 
             tag_next = tag_next->next) 
        {
            u_char *tag = (u_char *) tag_next->valuestring;
            if (tag == NULL) {
                continue;
            }
            if (ngx_strncmp(tag, "weight=", 7) == 0) {
                attr_value = ngx_atoi(tag + 7, (size_t)ngx_strlen(tag) - 7);

                if (attr_value == NGX_ERROR || attr_value <= 0) {
                    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                                  "upsync_parse_json: \"weight\" value is "
                                  "invalid, setting default value 1");
                    continue; 
                } else {
                    upstream_conf->weight = attr_value;
                }
            }
            if (ngx_strncmp(tag, "max_fails=", 10) == 0) {
                attr_value = ngx_atoi(tag + 10, (size_t)ngx_strlen(tag) - 10);

                if (attr_value == NGX_ERROR || attr_value < 0) {
                    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                                  "upsync_parse_json: \"max_fails\" value is "
                                  "invalid, setting default value 2");
                    continue; 
                } else {
                    upstream_conf->max_fails = attr_value;
                }
            }
            if (ngx_strncmp(tag, "fail_timeout=", 13) == 0) {
                ngx_str_t  value = {ngx_strlen(tag) - 13, tag + 13};
                attr_value = ngx_parse_time(&value, 1);

                if (attr_value == NGX_ERROR || attr_value < 0) {
                    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                                  "upsync_parse_json: \"fail_timeout\" value is "
                                  "invalid, setting default value 10");
                    continue; 
                } else {
                    upstream_conf->fail_timeout = attr_value;
                }
            }
            if (ngx_strncmp(tag, "down", 4) == 0 && tag[4] == '\0') {
                upstream_conf->down = 1;
            }
            if (ngx_strncmp(tag, "backup", 6) == 0 && tag[6] == '\0') {
                upstream_conf->backup = 1;
            }
        }
    }

    cJSON_Delete(root);

    return NGX_OK;
}

static ngx_int_t
ngx_http_upsync_consul_health_parse_json(void *data)
{
    ngx_buf_t                      *buf;
    ngx_http_upsync_ctx_t          *ctx;
    ngx_http_upsync_conf_t         *upstream_conf = NULL;
    ngx_http_upsync_server_t       *upsync_server = data;
    ngx_int_t                       attr_value;

    ctx = &upsync_server->ctx;
    buf = &ctx->body;

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
        cJSON_Delete(root);
        return NGX_ERROR;
    }

    cJSON *server_next;
    for (server_next = root->child; server_next != NULL;
         server_next = server_next->next)
    {
        cJSON *addr, *checks, *check_next, *node, *port, *service, *tags, *tag_next;
        size_t addr_len, port_len;
        u_char port_buf[8];

        service = cJSON_GetObjectItem(server_next, "Service");

        addr = cJSON_GetObjectItem(service, "Address");
        if (addr == NULL || addr->valuestring == NULL
            || addr->valuestring[0] == '\0')
        {
            node = cJSON_GetObjectItem(server_next, "Node");
            addr = cJSON_GetObjectItem(node, "Address");
            if (addr == NULL || addr->valuestring == NULL) {
                continue;
            }
        }

        port = cJSON_GetObjectItem(service, "Port");
        if (port == NULL || port->valueint < 1 || port->valueint > 65535) {
            continue;
        }
        ngx_memzero(port_buf, 8);
        ngx_sprintf(port_buf, "%d", port->valueint);

        addr_len = ngx_strlen(addr->valuestring);
        port_len = ngx_strlen(port_buf);

        if (addr_len + port_len + 2 > NGX_SOCKADDRLEN) {
            continue;
        }

        upstream_conf = ngx_array_push(&ctx->upstream_conf);
        if (upstream_conf == NULL) {
            cJSON_Delete(root);
            return NGX_ERROR;
        }
        ngx_memzero(upstream_conf, sizeof(*upstream_conf));

        ngx_memcpy(upstream_conf->sockaddr, addr->valuestring, addr_len);
        ngx_memcpy(upstream_conf->sockaddr + addr_len, ":", 1);
        ngx_memcpy(upstream_conf->sockaddr + addr_len + 1, port_buf, port_len);

        /* default value, server attribute */
        upstream_conf->weight = 1;
        upstream_conf->max_fails = 2;
        upstream_conf->fail_timeout = 10;

        upstream_conf->down = 0;
        upstream_conf->backup = 0;

        checks = cJSON_GetObjectItem(server_next, "Checks");

        for (check_next = checks->child; check_next != NULL;
             check_next = check_next->next)
        {
            cJSON *check_status;
            check_status = cJSON_GetObjectItem(check_next, "Status");

            if (check_status == NULL || check_status->valuestring == NULL
                || check_status->valuestring[0] == '\0'
                || ngx_strncmp(check_status->valuestring, "passing", 7) != 0)
            {
              upstream_conf->down = 1;
              break;
            }
        }

        tags = cJSON_GetObjectItem(service, "Tags");
        if (tags == NULL) {
            continue;
        }

        for (tag_next = tags->child; tag_next != NULL;
             tag_next = tag_next->next)
        {
            u_char *tag = (u_char *) tag_next->valuestring;
            if (tag == NULL) {
                continue;
            }
            if (ngx_strncmp(tag, "weight=", 7) == 0) {
                attr_value = ngx_atoi(tag + 7, (size_t)ngx_strlen(tag) - 7);

                if (attr_value == NGX_ERROR || attr_value <= 0) {
                    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                                  "upsync_parse_json: \"weight\" value is "
                                  "invalid, setting default value 1");
                    continue;
                } else {
                    upstream_conf->weight = attr_value;
                }
            }
            if (ngx_strncmp(tag, "max_fails=", 10) == 0) {
                attr_value = ngx_atoi(tag + 10, (size_t)ngx_strlen(tag) - 10);

                if (attr_value == NGX_ERROR || attr_value < 0) {
                    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                                  "upsync_parse_json: \"max_fails\" value is "
                                  "invalid, setting default value 2");
                    continue;
                } else {
                    upstream_conf->max_fails = attr_value;
                }
            }
            if (ngx_strncmp(tag, "fail_timeout=", 13) == 0) {
                ngx_str_t  value = {ngx_strlen(tag) - 13, tag + 13};
                attr_value = ngx_parse_time(&value, 1);

                if (attr_value == NGX_ERROR || attr_value < 0) {
                    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                                  "upsync_parse_json: \"fail_timeout\" value is "
                                  "invalid, setting default value 10");
                    continue;
                } else {
                    upstream_conf->fail_timeout = attr_value;
                }
            }
            if (ngx_strncmp(tag, "down", 4) == 0 && tag[4] == '\0') {
                upstream_conf->down = 1;
            }
            if (ngx_strncmp(tag, "backup", 6) == 0 && tag[6] == '\0') {
                upstream_conf->backup = 1;
            }
        }

    }

    cJSON_Delete(root);

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_etcd_parse_json(void *data)
{
    u_char                         *p;
    ngx_buf_t                      *buf;
    ngx_int_t                       max_fails=2, backup=0, down=0;
    ngx_http_upsync_ctx_t          *ctx;
    ngx_http_upsync_conf_t         *upstream_conf = NULL;
    ngx_http_upsync_server_t       *upsync_server = data;

    ctx = &upsync_server->ctx;
    buf = &ctx->body;

    cJSON *root = cJSON_Parse((char *)buf->pos);
    if (root == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_parse_json: root error");
        return NGX_ERROR;
    }
    
    cJSON *errorCode = cJSON_GetObjectItem(root, "errorCode");
    
    if (errorCode != NULL) {
        if (errorCode->valueint == 401) { // trigger reload, we've gone too far with index
            upsync_server->index = 0;

            ngx_del_timer(&upsync_server->upsync_timeout_ev);
            ngx_add_timer(&upsync_server->upsync_ev, 0);
        }
        cJSON_Delete(root);
        return NGX_ERROR;
    }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (action != NULL) {

        if (action->valuestring != NULL) {

            if (ngx_memcmp(action->valuestring, "get", 3) != 0) {
                upsync_server->index = 0;

                ngx_del_timer(&upsync_server->upsync_timeout_ev);
                ngx_add_timer(&upsync_server->upsync_ev, 0);

                cJSON_Delete(root);
                return NGX_ERROR;
            }
        }
    }
    action = NULL;

    if (ngx_array_init(&ctx->upstream_conf, ctx->pool, 16,
                       sizeof(*upstream_conf)) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_parse_json: array init error");
        cJSON_Delete(root);
        return NGX_ERROR;
    }

    cJSON *node = cJSON_GetObjectItem(root, "node");
    if (node == NULL) {
        cJSON_Delete(root);
        return NGX_ERROR;
    }

    cJSON *nodes = cJSON_GetObjectItem(node, "nodes");
    if (nodes == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_parse_json: nodes is null, no servers");
        cJSON_Delete(root);
        return NGX_ERROR;
    }

    cJSON *server_next;
    for (server_next = nodes->child; server_next != NULL; 
         server_next = server_next->next) 
    {
        cJSON *temp0 = cJSON_GetObjectItem(server_next, "key");
        if (temp0 != NULL && temp0->valuestring != NULL) {
            if (ngx_http_upsync_check_key((u_char *)temp0->valuestring, 
                                          upsync_server->host) != NGX_OK) {
                continue;
            }

            p = (u_char *)ngx_strrchr(temp0->valuestring, '/');
            upstream_conf = ngx_array_push(&ctx->upstream_conf);
            ngx_memzero(upstream_conf, sizeof(*upstream_conf));
            ngx_sprintf(upstream_conf->sockaddr, "%*s", ngx_strlen(p + 1), p + 1);
        }
        temp0 = NULL;

        /* default value, server attribute */
        upstream_conf->weight = 1;
        upstream_conf->max_fails = 2;
        upstream_conf->fail_timeout = 10;

        upstream_conf->down = 0;
        upstream_conf->backup = 0;

        temp0 = cJSON_GetObjectItem(server_next, "value");
        if (temp0 != NULL && ngx_strlen(temp0->valuestring) != 0) {

            cJSON *sub_attribute = cJSON_Parse((char *)temp0->valuestring);
            if (sub_attribute == NULL) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                              "upsync_parse_json: \'%s\' is invalid", 
                              temp0->valuestring);
                continue;
            }

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

        } else {
            continue;
        }

        if (upstream_conf->weight <= 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"weight\" value is invalid"
                          ", setting default value 1");
            upstream_conf->weight = 1;
        }

        if (max_fails < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"max_fails\" value is invalid"
                          ", setting default value 2");
        } else {
            upstream_conf->max_fails = (ngx_uint_t)max_fails;
        }

        if (upstream_conf->fail_timeout < 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"fail_timeout\" value is invalid"
                          ", setting default value 10");
            upstream_conf->fail_timeout = 10;
        }

        if (down != 1 && down != 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"down\" value is invalid"
                          ", setting default value 0");
        } else {
            upstream_conf->down = (ngx_uint_t)down;
        }

        if (backup != 1 && backup != 0) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "upsync_parse_json: \"backup\" value is invalid"
                          ", setting default value 0");
        } else {
            upstream_conf->backup = (ngx_uint_t)backup;
        }

        max_fails=2, backup=0, down=0;
    }
    cJSON_Delete(root);

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_check_key(u_char *key, ngx_str_t host)
{
    u_char          *last, *ip_p, *port_p, *s_p; //*u_p;
    ngx_int_t        port;
/*
    u_p = (u_char *)ngx_strstr(key, host.data);
    if (u_p == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_parse_json: %s is illegal, "
                      "dont contains subkey %V", key, &host);
        return NGX_ERROR;
    }
    if (*(u_p + host.len) != '/' || *(u_p - 1) != '/') {
        return NGX_ERROR;
    }
*/
    s_p = (u_char *)ngx_strrchr(key, '/');
    if (s_p == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_parse_json: %s is illegal, "
                      "dont contains slash \'/\'", key);
        return NGX_ERROR;
    }

    port_p = (u_char *)ngx_strchr(s_p, ':');
    if (port_p == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, 
                      "upsync_check_key: has no port in %s", s_p);
        return NGX_ERROR;
    }

    ip_p = s_p + 1;
    if (ngx_inet_addr(ip_p, port_p - ip_p) == INADDR_NONE) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, 
                      "upsync_check_key: invalid ip in %s", s_p);
        return NGX_ERROR;
    }

    last = ip_p + ngx_strlen(ip_p);
    port = ngx_atoi(port_p + 1, last - port_p - 1);
    if (port < 1 || port > 65535) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, 
                      "upsync_check_key: invalid port in %s", s_p);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void *
ngx_http_upsync_servers(ngx_cycle_t *cycle, 
    ngx_http_upsync_server_t *upsync_server, ngx_flag_t flag)
{
    ngx_uint_t                       i;
    ngx_addr_t                      *addrs;
    ngx_array_t                     *servers;  /* ngx_http_upstream_server_t */
    ngx_http_upsync_ctx_t           *ctx;
    ngx_http_upsync_conf_t          *conf;
    ngx_http_upstream_server_t      *server;

    ctx = &upsync_server->ctx;

    servers = ngx_pcalloc(ctx->pool, sizeof(ngx_array_t));
    if (servers == NULL) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_servers: alloc error");
        return NULL;
    }

    if (ngx_array_init(servers, ctx->pool, 16, sizeof(*server)) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                      "upsync_servers: alloc error");
        return NULL;
    }

    if (flag == NGX_ADD) {
        for (i = 0; i < ctx->add_upstream.nelts; i++) {
            conf = (ngx_http_upsync_conf_t *)ctx->add_upstream.elts + i;

            addrs = ngx_http_upsync_addrs(ctx->pool, conf->sockaddr);
            if (addrs == NULL) {
                continue;
            }

            server = ngx_array_push(servers);
            ngx_memzero(server, sizeof(ngx_http_upstream_server_t));

            server->addrs = addrs;
            server->naddrs = 1;
            server->down = conf->down;
            server->backup = conf->backup;
            server->weight = conf->weight;
            server->max_fails = conf->max_fails;
            server->fail_timeout = conf->fail_timeout;
        }

    } else {
        for (i = 0; i < ctx->del_upstream.nelts; i++) {
            conf = (ngx_http_upsync_conf_t *)ctx->del_upstream.elts + i;

            addrs = ngx_http_upsync_addrs(ctx->pool, conf->sockaddr);
            if (addrs == NULL) {
                continue;
            }

            server = ngx_array_push(servers);
            ngx_memzero(server, sizeof(ngx_http_upstream_server_t));

            server->addrs = addrs;
            server->naddrs = 1;
            server->down = conf->down;
            server->backup = conf->backup;
            server->weight = conf->weight;
            server->max_fails = conf->max_fails;
            server->fail_timeout = conf->fail_timeout;
        }
    }

    return servers;
}


static void *
ngx_http_upsync_addrs(ngx_pool_t *pool, u_char *sockaddr)
{
    u_char                 *port_p, *p, *last, *pp;
    ngx_int_t               port;
    ngx_addr_t             *addrs;

    struct sockaddr_in  *sin;

    p = sockaddr;
    last = p + ngx_strlen(p);

    port_p = ngx_strlchr(p, last, ':');
    if (port_p == NULL) {
        ngx_log_error(NGX_LOG_ERR, pool->log, 0, 
                      "upsync_addrs: has no port in %s", p);
        return NULL;
    }

    port = ngx_atoi(port_p + 1, last - port_p - 1);
    if (port < 1 || port > 65535) {
        ngx_log_error(NGX_LOG_ERR, pool->log, 0, 
                      "upsync_addrs: invalid port in %s", p);
        return NULL;
    }

    sin = ngx_pcalloc(pool, sizeof(struct sockaddr_in));
    if (sin == NULL) {
        return NULL;
    }

    sin->sin_family = AF_INET;
    sin->sin_port = htons((in_port_t) port);
    sin->sin_addr.s_addr = ngx_inet_addr(p, port_p - p);

    if (sin->sin_addr.s_addr == INADDR_NONE) {
        ngx_log_error(NGX_LOG_ERR, pool->log, 0, 
                      "upsync_addrs: invalid ip in %s", p);
        return NULL;
    }

    addrs = ngx_pcalloc(pool, sizeof(ngx_addr_t));
    if (addrs == NULL) {
        return NULL;
    }

    addrs->sockaddr = (struct sockaddr *) sin;
    addrs->socklen = sizeof(struct sockaddr_in);

    pp = ngx_pcalloc(pool, last - p);
    if (pp == NULL) {
        return NULL;
    }
    addrs->name.len = ngx_sprintf(pp, "%s", p) - pp;
    addrs->name.data = pp;

    return addrs;
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

    upscf->upsync_lb = NGX_CONF_UNSET;

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

    if (upscf->upsync_lb == NGX_CONF_UNSET) {
        upscf->upsync_lb = NGX_HTTP_LB_DEFAULT;
    }

    if (upscf->strong_dependency == NGX_CONF_UNSET_UINT) {
        upscf->strong_dependency = 0;
    }

    if (upscf->upsync_dump_path.len == NGX_CONF_UNSET_SIZE) {
        buf = ngx_pcalloc(cf->pool, 
                          ngx_strlen("/tmp/servers_.conf") + uscf->host.len + 1);
        ngx_sprintf(buf, "/tmp/servers_%V.conf", &uscf->host);

        upscf->upsync_dump_path.data = buf;
        upscf->upsync_dump_path.len = ngx_strlen("/tmp/servers_.conf")
                                      + uscf->host.len;
    }

    upscf->conf_file = ngx_pcalloc(cf->pool, sizeof(ngx_open_file_t));
    if (upscf->conf_file == NULL) {
        return NGX_CONF_ERROR; 
    }
    upscf->conf_file->fd = NGX_INVALID_FILE;
    upscf->conf_file->name = upscf->upsync_dump_path;
    upscf->conf_file->flush = NULL;
    upscf->conf_file->data = NULL;

    upsync_server->index = 0;
    upsync_server->update_generation = 0;

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

    // no http {} block found
    if (upsync_ctx == NULL) {
        return NGX_OK;
    }
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
    ngx_int_t                            status = 0;
    ngx_uint_t                           i, j;
    ngx_pool_t                          *pool;
    ngx_upsync_conf_t                   *upsync_type_conf;
    ngx_http_upsync_ctx_t               *ctx;
    ngx_http_upsync_server_t            *upsync_server;

    // no http {} block found
    if (upsync_ctx == NULL) {
        return NGX_OK;
    }
    upsync_server = upsync_ctx->upsync_server;

    for (i = 0; i < upsync_ctx->upstream_num; i++) {

        ngx_queue_init(&upsync_server[i].delete_ev);
        if (upsync_server[i].upscf->strong_dependency == 0) {
            continue;
        }

        ctx = &upsync_server[i].ctx;
        ngx_memzero(ctx, sizeof(*ctx));
        upsync_type_conf = upsync_server[i].upscf->upsync_type_conf;

        pool = ngx_create_pool(NGX_DEFAULT_POOL_SIZE, ngx_cycle->log);
        if (pool == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                          "upsync_init_process: recv error, "
                          "server no enough memory");
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
                          "upsync_init_process: pull upstream \"%V\" conf failed",
                          &upsync_server->host);
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

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cycle->log, 0, "upsync_add_timers");

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
        upscf = upsync_server[i].upscf;
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
        parser = NULL;
    }

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
                      "upsync_connect_handler: cannot connect to upsync_server: %V ",
                      upsync_server->pc.name);

        ngx_del_timer(&upsync_server->upsync_timeout_ev);
        ngx_add_timer(&upsync_server->upsync_ev, 0);

        return;
    }

    /* NGX_OK or NGX_AGAIN */
    c = upsync_server->pc.connection;
    c->data = upsync_server;
    c->log = upsync_server->pc.log;
    c->sendfile = 0;
    c->read->log = c->log;
    c->write->log = c->log;

    c->idle = 1; //for quick exit.

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

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL_SERVICES
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL_HEALTH)
    {
        ngx_sprintf(request, "GET %V?recurse&index=%uL HTTP/1.0\r\nHost: %V\r\n"
                    "Accept: */*\r\n\r\n", 
                    &upscf->upsync_send, upsync_server->index, 
                    &upscf->upsync_host);
    }

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_ETCD) {
        if (upsync_server->index != 0) {
            ngx_sprintf(request, "GET %V?wait=true&recursive=true&waitIndex=%uL"
                        " HTTP/1.0\r\nHost: %V\r\nAccept: */*\r\n\r\n", 
                        &upscf->upsync_send, upsync_server->index, 
                        &upscf->upsync_host);

        } else {
            ngx_sprintf(request, "GET %V?" 
                        " HTTP/1.0\r\nHost: %V\r\nAccept: */*\r\n\r\n", 
                        &upscf->upsync_send, &upscf->upsync_host);

        }
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

    c->write->handler = ngx_http_upsync_send_empty_handler;

    return;

upsync_send_fail:
    ngx_log_error(NGX_LOG_ERR, event->log, 0,
                  "upsync_send: send upstream \"%V\" error",
                  &upsync_server->host);

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
                          "upsync_recv: recv not enough memory");
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

    if (upsync_type_conf->init(upsync_server) == NGX_OK) {
        ngx_http_upsync_process(upsync_server);

        c->read->handler = ngx_http_upsync_recv_empty_handler;
    }

    upsync_type_conf->clean(upsync_server);

    return;

upsync_recv_fail:
    ngx_log_error(NGX_LOG_ERR, event->log, 0,
                  "upsync_recv: recv error with upstream: \"%V\"",
                  &upsync_server->host);

    ngx_http_upsync_clean_event(upsync_server);
}


static void
ngx_http_upsync_send_empty_handler(ngx_event_t *event)
{
    /* void */
}


static void
ngx_http_upsync_recv_empty_handler(ngx_event_t *event)
{
    /* void */
}


static ngx_int_t
ngx_http_upsync_consul_parse_init(void *data)
{
    char                                  *buf;
    size_t                                 parsed;
    ngx_http_upsync_ctx_t                 *ctx;
    ngx_http_upsync_server_t              *upsync_server = data;

    ctx = &upsync_server->ctx;

    if (ngx_http_parser_init() == NGX_ERROR) {
        return NGX_ERROR;
    }

    buf = (char *)ctx->recv.pos;
    ctx->body.pos = ctx->body.last = NULL;

    parsed = http_parser_execute(parser, &settings, buf, ngx_strlen(buf));
    if (parsed != ngx_strlen(buf)) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_consul_parse_init: parsed upstream \"%V\" wrong",
                      &upsync_server->host);

        if (parser != NULL) {
            ngx_free(parser);
            parser = NULL;
        }

        return NGX_ERROR;
    }

    if (ngx_strncmp(state.status, "OK", 2) == 0) {

        if (ngx_strlen(state.http_body) != 0) {
            ctx->body.pos = state.http_body;
            ctx->body.last = state.http_body + ngx_strlen(state.http_body);

            *(ctx->body.last + 1) = '\0';
        }
    } else {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_consul_parse_init: recv upstream \"%V\" error; "
                      "http_status: %d", &upsync_server->host, parser->status_code);

        if (parser != NULL) {
            ngx_free(parser);
            parser = NULL;
        }

        return NGX_ERROR;
    }

    if (parser != NULL) {
        ngx_free(parser);
        parser = NULL;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_etcd_parse_init(void *data)
{
    char                                  *buf;
    size_t                                 parsed;
    ngx_http_upsync_ctx_t                 *ctx;
    ngx_http_upsync_server_t              *upsync_server = data;

    ctx = &upsync_server->ctx;

    if (ngx_http_parser_init() == NGX_ERROR) {
        return NGX_ERROR;
    }

    buf = (char *)ctx->recv.pos;
    ctx->body.pos = ctx->body.last = NULL;

    parsed = http_parser_execute(parser, &settings, buf, ngx_strlen(buf));
    if (parsed != ngx_strlen(buf)) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_consul_parse_init: parsed upstream \"%V\" wrong",
                      &upsync_server->host);

        if (parser != NULL) {
            ngx_free(parser);
            parser = NULL;
        }

        return NGX_ERROR;
    }

    if (ngx_strncmp(state.status, "OK", 2) == 0
        || ngx_strncmp(state.status, "Bad", 3) == 0) {

        if (ngx_strlen(state.http_body) != 0) {
            ctx->body.pos = state.http_body;
            ctx->body.last = state.http_body + ngx_strlen(state.http_body);

            *(ctx->body.last + 1) = '\0';
        }

    } else {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                      "upsync_consul_parse_init: recv upstream \"%V\" error; "
                      "http_status: %d", &upsync_server->host, parser->status_code);

        if (parser != NULL) {
            ngx_free(parser);
            parser = NULL;
        }

        return NGX_ERROR;
    }

    if (parser != NULL) {
        ngx_free(parser);
        parser = NULL;
    }
    
    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_dump_server(ngx_http_upsync_server_t *upsync_server)
{
    ngx_buf_t                               *b=NULL;
    ngx_http_upsync_srv_conf_t              *upscf = NULL;
    ngx_http_upstream_rr_peer_t             *peer = NULL;
    ngx_http_upstream_rr_peers_t            *peers = NULL;
    ngx_http_upstream_srv_conf_t            *uscf = NULL;

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
                      "upsync_dump_server: there are no peers to dump");
        return NGX_ERROR;
    }

    b = ngx_create_temp_buf(upsync_server->ctx.pool, 
                            NGX_PAGE_SIZE * NGX_PAGE_NUMBER);
    if (b == NULL) {
        ngx_log_error(NGX_LOG_ERR, upsync_server->ctx.pool->log, 0,
                      "upsync_dump_server: dump failed %V", &uscf->host);
        return NGX_ERROR;
    }

    for (peer = peers->peer; peer; peer = peer->next) {
        b->last = ngx_snprintf(b->last, b->end - b->last, 
                               "server %V", &peer->name);
        b->last = ngx_snprintf(b->last, b->end - b->last, 
                               " weight=%d", peer->weight);
        b->last = ngx_snprintf(b->last, b->end - b->last, 
                               " max_fails=%d", peer->max_fails);
        b->last = ngx_snprintf(b->last, b->end - b->last, 
                               " fail_timeout=%ds", peer->fail_timeout);

        if (peer->down) {
            b->last = ngx_snprintf(b->last, b->end - b->last, " down");
        }
 
        b->last = ngx_snprintf(b->last, b->end - b->last, ";\n");
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
                  "upsync_dump_server: dump conf file %V succeeded, number of servers is %d", 
                  &upscf->upsync_dump_path, peers->number);

    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_init_server(ngx_event_t *event)
{
    ngx_uint_t                               n = 0, r = 0, cur = 0;
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
                          "upsync_init_consul: cannot create pool, not enough memory");
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

        /* bad method: for get random server*/
        for (rp = res; rp != NULL; rp = rp->ai_next) {
            if (rp->ai_family != AF_INET) {
                continue;
            }

            n++;
        }

        r = ngx_random() % n;
        for (rp = res; rp != NULL; rp = rp->ai_next) {
            if (rp->ai_family != AF_INET) {
                continue;
            }

            if (cur != r) {
                cur++;
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
ngx_http_upsync_event_init(ngx_http_upstream_rr_peer_t *peer, 
    ngx_http_upsync_server_t *upsync_server)
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

    delay_event->delay_delete_ev.handler = ngx_http_upsync_del_delay_delete;
    delay_event->delay_delete_ev.log = ngx_cycle->log;
    delay_event->delay_delete_ev.data = delay_event;
    delay_event->delay_delete_ev.timer_set = 0;

    ngx_queue_insert_head(&upsync_server->delete_ev, &delay_event->queue);

    delay_event->data = peer;
    ngx_add_timer(&delay_event->delay_delete_ev, NGX_DELAY_DELETE);

    return;
}


static void
ngx_http_upsync_del_delay_delete(ngx_event_t *event)
{
    ngx_msec_t                       t;
    ngx_uint_t                       i, conn_interval;
    ngx_connection_t                *c;
    ngx_delay_event_t               *delay_event;
    ngx_http_request_t              *r = NULL;
    ngx_http_log_ctx_t              *ctx = NULL;
    ngx_http_upstream_rr_peer_t     *peer = NULL, *pre_peer = NULL;

    u_char *namep = NULL;
    struct sockaddr *saddr = NULL;

    delay_event = event->data;
    if (delay_event == NULL) {
        return;
    }
    peer = delay_event->data;

    c = ngx_cycle->connections;
    conn_interval = ngx_cycle->connection_n / 30;
    if (!conn_interval) {
        conn_interval = 1;
    }
    for (i = 0; i < ngx_cycle->connection_n; i += conn_interval) {

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
                t = ngx_random() % NGX_DELAY_DELETE + NGX_DELAY_DELETE;
                ngx_add_timer(&delay_event->delay_delete_ev, t);
                return;
            }

            if (r->start_sec == delay_event->start_sec) {

                if (r->start_msec <= delay_event->start_msec) {
                    t = ngx_random() % NGX_DELAY_DELETE + NGX_DELAY_DELETE;
                    ngx_add_timer(&delay_event->delay_delete_ev, t);
                    return;
                }
            }
        }
    }

    while (peer != NULL) {
        saddr = peer->sockaddr;
        if (saddr != NULL) {
            ngx_free(saddr);
            saddr = NULL;
        }

        namep = peer->name.data;
        if (namep != NULL) {
            ngx_free(namep);
            namep = NULL;
        }

        pre_peer = peer;
        peer = peer->next;

        ngx_free(pre_peer);
    }

    ngx_queue_remove(&delay_event->queue);
    ngx_free(delay_event);
    delay_event = NULL;

    return;
}


static size_t
ngx_http_upsync_strnlen(const char *s, size_t maxlen)
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

    slen = ngx_http_upsync_strnlen(src, n);
    dlen = ngx_http_upsync_strnlen(dst, len);

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
                      "ngx_http_parser_init: ngx_calloc failed");
        return NGX_ERROR;
    }

    http_parser_init(parser, HTTP_RESPONSE);

    return NGX_OK;
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
                  "[WARN] upsync_timeout: timed out reading upsync_server: %V ",
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

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL_SERVICES
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL_HEALTH
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_ETCD)
    {
        if (parser != NULL) {
            ngx_free(parser);
            parser = NULL;
        }
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
        ngx_http_upsync_clear_all_events((ngx_cycle_t *)ngx_cycle);

        return 1;
    }

    return NGX_OK;
}


static void
ngx_http_upsync_clear_all_events(ngx_cycle_t *cycle)
{
    ngx_uint_t                          i;
    ngx_queue_t                        *head, *next;
    ngx_connection_t                   *c;
    ngx_delay_event_t                  *queue_event;
    ngx_upsync_conf_t                  *upsync_type_conf;
    ngx_http_upsync_server_t           *upsync_server;

    static ngx_flag_t                   has_cleared = 0;

    if (has_cleared || upsync_ctx == NULL || upsync_ctx->upstream_num == 0) {
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

        head = &upsync_server[i].delete_ev;
        for (next = ngx_queue_head(head);
                next != ngx_queue_sentinel(head);
                next = ngx_queue_next(next)) {

            queue_event = ngx_queue_data(next, ngx_delay_event_t, queue);
            if (queue_event->delay_delete_ev.timer_set) {
                ngx_del_timer(&queue_event->delay_delete_ev);
            }
        }
    }

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL_SERVICES
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL_HEALTH
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_ETCD) {

        if (parser != NULL) {
            ngx_free(parser);
            parser = NULL;
        }
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
            response = NULL;
        }
        ngx_http_client_destroy(client);

        return NGX_ERROR;
    }
    ngx_http_client_destroy(client);

    if (ngx_http_parser_init() == NGX_ERROR) {
        return NGX_ERROR;
    }
    http_parser_execute(parser, &settings, response, ngx_strlen(response));
    if (parser != NULL) {
        ngx_free(parser);
        parser = NULL;
    }
    if (ngx_strncmp(state.status, "OK", 2) != 0) {
        return NGX_ERROR;
    }

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

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL_SERVICES
        || upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_CONSUL_HEALTH)
    {
        ngx_sprintf(request, "GET %V?recurse&index=%uL HTTP/1.0\r\nHost: %V\r\n"
                    "Accept: */*\r\n\r\n", 
                    &upscf->upsync_send, upsync_server->index, 
                    &upscf->conf_server.name);
    }

    if (upsync_type_conf->upsync_type == NGX_HTTP_UPSYNC_ETCD) {
        ngx_sprintf(request, "GET %V? HTTP/1.0\r\nHost: %V\r\n"
                    "Accept: */*\r\n\r\n", 
                    &upscf->upsync_send, &upscf->conf_server.name);
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
    char        *tmp_data;
    char         buff[ngx_pagesize];
    ssize_t      recv_num = 0, tmp_recv = 0;
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
                ngx_free(tmp_data);
                return NGX_ERROR;
            }
            ngx_memcpy(*data, tmp_data, recv_num - tmp_recv);

            ngx_free(tmp_data);
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


static void 
ngx_http_upsync_show_upstream(ngx_http_upstream_srv_conf_t *uscf, ngx_buf_t *b)
{
    ngx_str_t                       *host;
    ngx_http_upstream_rr_peer_t     *peer = NULL;
    ngx_http_upstream_rr_peers_t    *peers = NULL;

    host = &(uscf->host);

    b->last = ngx_snprintf(b->last, b->end - b->last,
                           "Upstream name: %V; ", host);    

    if (uscf->peer.data == NULL) {
        b->last = ngx_snprintf(b->last, b->end - b->last,
                               "Backend server count: %d\n", 0);
        return;
    }

    peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;

    b->last = ngx_snprintf(b->last, b->end - b->last,
                           "Backend server count: %d\n", peers->number);

    for (peer = peers->peer; peer; peer = peer->next) {
        b->last = ngx_snprintf(b->last, b->end - b->last, 
                               "        server %V", &peer->name);
        b->last = ngx_snprintf(b->last, b->end - b->last, 
                               " weight=%d", peer->weight);
        b->last = ngx_snprintf(b->last, b->end - b->last, 
                               " max_fails=%d", peer->max_fails);
        b->last = ngx_snprintf(b->last, b->end - b->last, 
                               " fail_timeout=%ds", peer->fail_timeout);

        if (peer->down) {
            b->last = ngx_snprintf(b->last, b->end - b->last, " down");
        }

        b->last = ngx_snprintf(b->last, b->end - b->last, ";\n");
    }
}


static ngx_int_t
ngx_http_upsync_show(ngx_http_request_t *r)
{
    ngx_buf_t                             *b;
    ngx_int_t                              rc, ret;
    ngx_str_t                             *host;
    ngx_uint_t                             i;
    ngx_chain_t                            out;
    ngx_http_upstream_srv_conf_t         **uscfp = NULL;
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

        if (umcf->upstreams.nelts == 0) {
            b->last = ngx_snprintf(b->last, b->end - b->last,
                                   "No upstreams defined");

            goto end;
        }
    	
    	for (i = 0; i < umcf->upstreams.nelts; i++) {
            ngx_http_upsync_show_upstream(uscfp[i], b);
            b->last = ngx_snprintf(b->last, b->end - b->last, "\n");
        }
    	
        goto end;
    }

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        if (uscfp[i]->host.len == host->len
            && ngx_strncasecmp(uscfp[i]->host.data, host->data, host->len) == 0) 
        {
            ngx_http_upsync_show_upstream(uscfp[i], b);
            goto end;
        }
    }

    b->last = ngx_snprintf(b->last, b->end - b->last, 
                           "The upstream you requested does not exist. "
                           "Please double-check the name");    

end:
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    b->last_buf = (r == r->main) ? 1 : 0;

    r->connection->buffered |= NGX_HTTP_WRITE_BUFFERED;
    ret = ngx_http_send_header(r);
    ret = ngx_http_output_filter(r, &out);
 
    return ret;
}
