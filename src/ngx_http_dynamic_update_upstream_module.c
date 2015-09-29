/*
 * Copyright (C) 2015 Weibo Group Holding Limited
 * Copyright (C) 2015 Xiaokai Wang (xiaokai.wang@live.com)
 */


#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_config.h>

#include "ngx_http_json.h"
#include "ngx_http_parser.h"

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

    ngx_uint_t                       weight;
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

    ngx_array_t                      del_upstream;
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

    ngx_http_upstream_server_t    us;
} ngx_http_dynamic_update_upstream_srv_conf_t;


typedef struct {
    ngx_event_t                              update_ev;
    ngx_event_t                              update_timeout_ev;

    ngx_queue_t                              add_ev;
    ngx_queue_t                              delete_ev;

    ngx_int_t                                index;

    ngx_str_t                                host;

    ngx_flag_t                               update_label;

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
extern ngx_uint_t ngx_http_upstream_check_add_dynamic_peer(ngx_pool_t *pool,
        ngx_http_upstream_srv_conf_t *us, ngx_addr_t *peer_addr);
extern void ngx_http_upstream_check_delete_dynamic_peer(ngx_str_t *name,
        ngx_addr_t *peer_addr);
#endif

static char * ngx_http_dynamic_update_upstream_consul_server(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf);

static void *ngx_http_dynamic_update_upstream_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_dynamic_update_upstream_create_srv_conf(ngx_conf_t *cf);
static char *ngx_http_dynamic_update_upstream_init_main_conf(ngx_conf_t *cf, void *conf);
static char *ngx_http_dynamic_update_upstream_init_srv_conf(ngx_conf_t *cf, void *conf, 
        ngx_uint_t num);

static void ngx_http_dynamic_update_upstream_process(
        ngx_http_dynamic_update_upstream_server_t *conf_server);

static ngx_int_t ngx_http_dynamic_update_upstream_init_process(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_dynamic_update_upstream_add_timers(ngx_cycle_t *cycle);

static void ngx_http_dynamic_update_upstream_begin_handler(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_connect_handler(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_recv_handler(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_send_handler(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_timeout_handler(ngx_event_t *event);
static void ngx_http_dynamic_update_upstream_clean_event(
        ngx_http_dynamic_update_upstream_server_t *peer);
static ngx_int_t ngx_http_dynamic_update_upstream_init_peer(ngx_event_t *event);

static ngx_int_t ngx_http_dynamic_update_upstream_add_server(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server);
static ngx_int_t ngx_http_dynamic_update_upstream_copy_peer(ngx_cycle_t *cycle,
        ngx_http_upstream_srv_conf_t *uscf, ngx_http_upstream_server_t *us,
        ngx_http_dynamic_update_upstream_server_t *conf_server);
static ngx_int_t ngx_http_dynamic_update_upstream_add_peer(ngx_cycle_t *cycle, 
        ngx_http_upstream_srv_conf_t *uscf, ngx_http_upstream_server_t  *us);
static void ngx_http_dynamic_update_upstream_add_check(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server);

static ngx_int_t ngx_http_dynamic_update_upstream_del_server(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server);
static ngx_int_t ngx_http_dynamic_update_upstream_del_peer(ngx_cycle_t *cycle,
        ngx_http_upstream_srv_conf_t *uscf, ngx_http_upstream_server_t *ctx,
        ngx_http_dynamic_update_upstream_server_t *conf_server);
static void ngx_http_dynamic_update_upstream_del_check(ngx_cycle_t *cycle, 
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

static http_parser *parser = NULL;
static ngx_http_state state;
static ngx_int_t update_flag = 0;

static ngx_http_dynamic_update_upstream_main_conf_t  *dynamic_upstream_ctx = NULL;

static ngx_command_t  ngx_http_dynamic_update_upstream_commands[] = {

    {  ngx_string("consul"),
        NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
        ngx_http_dynamic_update_upstream_consul_server,
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

    NULL,                                              /* create location configuration */
    NULL                                               /* merge main configuration */
};


ngx_module_t  ngx_http_dynamic_update_upstream_module = {
    NGX_MODULE_V1,
    &ngx_http_dynamic_update_upstream_module_ctx, /* module context */
    ngx_http_dynamic_update_upstream_commands,    /* module directives */
    NGX_HTTP_MODULE,                              /* module type */
    NULL,                                         /* init master */
    NULL,                                         /* init module */
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

            update_timeout = ngx_parse_time(&s, 1);

            if (update_timeout == (time_t) NGX_ERROR) {
                goto invalid;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "update_interval=", 16) == 0) {

            s.len = value[i].len - 16;
            s.data = &value[i].data[16];

            update_interval = ngx_parse_time(&s, 1);

            if (update_interval == (time_t) NGX_ERROR) {
                goto invalid;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "delay_delete=", 13) == 0) {

            s.len = value[i].len - 13;
            s.data = &value[i].data[13];

            delay_delete = ngx_parse_time(&s, 1);

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
        duscf->update_interval = update_interval * 1000;
    }

    if (update_timeout != 0) {
        duscf->update_timeout = update_timeout * 1000;
    }

    if (delay_delete != 0) {
        duscf->delay_delete = delay_delete * 1000;
    }

    if (strong_dependency != 0) {
        duscf->strong_dependency = strong_dependency;
    }

    ngx_memzero(&u, sizeof(ngx_url_t));

    p = (u_char *)ngx_strchr(value[1].data, '/');
    if (p == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "consul_server: please input consul upstream key in upstream");
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
                                "consul_server: consul server port is invalid");
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
                               "%s in upstream \"%V\"", u.err, &u.url);
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
                       "consul invalid parameter \"%V\"", &value[i]);

    return NGX_CONF_ERROR;
}


static void
ngx_http_dynamic_update_upstream_process(ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    char                                         *p;
    ngx_buf_t                                    *buf;
    ngx_int_t                                     index = 0;
    ngx_uint_t                                    i;
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
        ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                "dynamic update upstream index is not change in upstream: %V",
                &conf->consul_host);
        return;

    } else {
        conf_server->index = index;
    }

    if (ngx_http_dynamic_update_upstream_parse_json(buf->pos, 
                conf_server) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "dynamic update upstream parse json error ");
        return;
    }

    ngx_log_debug0(NGX_LOG_DEBUG, ngx_cycle->log, 0,
            "dynamic update upstream parse json succeed");

    ngx_http_dynamic_update_upstream_add_check((ngx_cycle_t *)ngx_cycle, conf_server);

    if (ctx->add_upstream.nelts > 0) {

        if (ngx_http_dynamic_update_upstream_add_server((ngx_cycle_t *)ngx_cycle, 
                    conf_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                    "ngx_http_dynamic_update: upstream_add_server error ");
            return;
        }
    }

    ngx_http_dynamic_update_upstream_del_check((ngx_cycle_t *)ngx_cycle, conf_server);

    if (ctx->del_upstream.nelts > 0) {

        if (ngx_http_dynamic_update_upstream_del_server((ngx_cycle_t *)ngx_cycle, 
                    conf_server) != NGX_OK) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                    "ngx_http_dynamic_update: upstream_del_server error ");
            return;
        }
    }

    return;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_add_server(ngx_cycle_t *cycle, 
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    time_t                                   fail_timeout = 0;
    u_char                                  *port, *p, *last, *pp;
    ngx_int_t                                n, j;
    ngx_uint_t                               i, weight = 0, max_fails = 0;
    ngx_uint_t                               down = 0, backup = 0;
    ngx_addr_t                              *addrs;
    ngx_http_update_conf_t                  *conf;
    ngx_http_upstream_srv_conf_t            *uscf;
    ngx_http_upstream_server_t               us;
    ngx_http_dynamic_update_upstream_ctx_t  *ctx;

    struct sockaddr_in  *sin;

    uscf = conf_server->uscf;

    ctx = &conf_server->ctx;

    ngx_memzero(&us, sizeof(ngx_http_upstream_server_t));

    addrs = ngx_calloc(ctx->add_upstream.nelts * sizeof(ngx_addr_t), cycle->log);
    if (addrs == NULL) {
        return NGX_ERROR;
    }

    for (i = 0, j = 0; i < ctx->add_upstream.nelts; i++, j++) {
        conf = (ngx_http_update_conf_t *)ctx->add_upstream.elts + i;

        p = conf->sockaddr;
        last = p + ngx_strlen(p);

        port = ngx_strlchr(p, last, ':');
        if (port == NULL) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "upstream_add_server: has no port in %s", p);
            j--;
            continue;
        }

        n = ngx_atoi(port + 1, last - port - 1);
        if (n < 1 || n > 65535) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "upstream_add_server: invalid port in %s", p);
            j--;
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
                    "upstream_add_server: invalid ip in %s", p);
            j--;

            ngx_free(sin);
            continue;
        }

        addrs[j].sockaddr = (struct sockaddr *) sin;
        addrs[j].socklen = sizeof(struct sockaddr_in);

        pp = ngx_calloc(last - p, cycle->log);
        if (pp == NULL) {
            goto invalid;
        }

        addrs[j].name.len = ngx_sprintf(pp, "%s", p) - pp;
        addrs[j].name.data = pp;

        if (conf->weight != 0) {
            weight = conf->weight;
        }
        if (conf->max_fails != 0) {
            max_fails = conf->max_fails;
        }
        if (conf->fail_timeout != 0) {
            fail_timeout = conf->fail_timeout;
        }
        if (conf->down != 0) {
            down = conf->down;
        }
        if (conf->backup != 0) {
            backup = conf->backup;
        }
    }

    us.down = down;
    us.backup = backup;

    us.addrs = addrs;
    us.naddrs = j;

    if (weight != 0) {
        us.weight = weight;
    } else {
        us.weight = 1;
    }

    if (max_fails != 0) {
        us.max_fails = max_fails;
    } else {
        us.max_fails = 2;
    }

    if (fail_timeout != 0) {
        us.fail_timeout = fail_timeout;
    } else {
        us.fail_timeout = 10;
    }

    if (us.naddrs > 0) {
        if (ngx_http_dynamic_update_upstream_copy_peer(cycle, uscf, 
                    &us, conf_server) 
                != NGX_OK) 
        {
            goto invalid;
        }

        if (ngx_http_dynamic_update_upstream_add_peer(cycle, uscf, 
                    &us) != NGX_OK) {
            goto invalid;
        }
    }

    return NGX_OK;

invalid:

    for (i = 0; i < ctx->add_upstream.nelts; i++) {
        if (addrs[i].sockaddr != NULL) {
            ngx_free(addrs[i].sockaddr);
        }

        if (addrs[i].name.data != NULL) {
            ngx_free(addrs[i].name.data);
        }
    }
    ngx_free(addrs);
    addrs = NULL;

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_copy_peer(ngx_cycle_t *cycle,
        ngx_http_upstream_srv_conf_t *uscf, ngx_http_upstream_server_t *us,
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    ngx_uint_t                                   i, n, m, len, num;
    ngx_flag_t                                   update_label;
    ngx_http_upstream_rr_peers_t                *peers=NULL, *backup=NULL;
    ngx_http_upstream_rr_peers_t                *tmp_peers=NULL, *tmp_backup=NULL;

    update_label = conf_server->update_label;

    u_char *namep = NULL;
    struct sockaddr *saddr = NULL;
    len = sizeof(struct sockaddr);

    num = us->naddrs;

    if (uscf->peer.data != NULL) {
        tmp_peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
        tmp_backup = tmp_peers->next;
    }

    if (tmp_peers) {
        n = tmp_peers->number;
        m = n + num;

        peers = ngx_calloc(sizeof(ngx_http_upstream_rr_peers_t)
                + sizeof(ngx_http_upstream_rr_peer_t) * (m - 1), cycle->log);

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

            if (!update_label) {

                if ((saddr = ngx_calloc(len, cycle->log)) == NULL) {
                    goto invalid;
                }
                ngx_memcpy(saddr, tmp_peers->peer[n].sockaddr, len);
                peers->peer[n].sockaddr = saddr;

            } else {
                peers->peer[n].sockaddr = tmp_peers->peer[n].sockaddr;
            }

            peers->peer[n].socklen = tmp_peers->peer[n].socklen;
            peers->peer[n].name.len = tmp_peers->peer[n].name.len;

            if (!update_label) {

                if ((namep = ngx_calloc(tmp_peers->peer[n].name.len,
                                        cycle->log)) == NULL) {
                    goto invalid;
                }
                ngx_memcpy(namep, tmp_peers->peer[n].name.data,
                           tmp_peers->peer[n].name.len);
                peers->peer[n].name.data = namep;
            } else {

                peers->peer[n].name.data = tmp_peers->peer[n].name.data;
            }

            peers->peer[n].max_fails = tmp_peers->peer[n].max_fails;
            peers->peer[n].fail_timeout = tmp_peers->peer[n].fail_timeout;
            peers->peer[n].down = tmp_peers->peer[n].down;
            peers->peer[n].weight = tmp_peers->peer[n].weight;
            peers->peer[n].effective_weight = tmp_peers->peer[n].effective_weight;
            peers->peer[n].current_weight = tmp_peers->peer[n].current_weight;

#if (NGX_HTTP_UPSTREAM_CHECK) 
            peers->peer[n].check_index = tmp_peers->peer[n].check_index;
#endif

            n++;
        }

        uscf->peer.data = peers;

        /* backup servers */

        if (tmp_backup) {
            n = tmp_backup->number;

        } else {
            n = 0;
        }

        if (n == 0) {
            if (update_label) {
                ngx_http_dynamic_update_upstream_event_init(tmp_peers, conf_server, NGX_ADD);
            }

            return NGX_OK;
        }

        backup = ngx_calloc(sizeof(ngx_http_upstream_rr_peers_t)
                + sizeof(ngx_http_upstream_rr_peer_t) * (n - 1), cycle->log);
        if (backup == NULL) {
            goto invalid;
        }

        peers->single = 0;
        backup->single = tmp_backup->single;
        backup->number = tmp_backup->number;
        backup->weighted = tmp_backup->weighted;
        backup->total_weight = tmp_backup->total_weight;
        backup->name = tmp_backup->name;

        n = 0;

        for (i = 0; i < uscf->servers->nelts; i++) {

            backup->peer[n].sockaddr = tmp_backup->peer[n].sockaddr;
            backup->peer[n].socklen = tmp_backup->peer[n].socklen;
            backup->peer[n].name.len = tmp_backup->peer[n].name.len;
            backup->peer[n].name.data = tmp_backup->peer[n].name.data;
            backup->peer[n].weight = tmp_backup->peer[n].weight;
            backup->peer[n].effective_weight = tmp_backup->peer[n].weight;
            backup->peer[n].current_weight = tmp_backup->peer[n].current_weight;
            backup->peer[n].max_fails = tmp_backup->peer[n].max_fails;
            backup->peer[n].fail_timeout = tmp_backup->peer[n].fail_timeout;
            backup->peer[n].down = tmp_backup->peer[n].down;

#if (NGX_HTTP_UPSTREAM_CHECK) 
            backup->peer[n].check_index = tmp_backup->peer[n].check_index;
#endif

            n++;
        }

        peers->next = backup;

        if (update_label) {
            ngx_http_dynamic_update_upstream_event_init(tmp_peers, conf_server, NGX_ADD);
        }

        return NGX_OK;
    }

    return NGX_OK;

invalid:
    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
            "upstream_add_server copy failed \"%V\" in %s:%ui",
            &uscf->host, uscf->file_name, uscf->line);

    if (peers != NULL) {
        ngx_http_dynamic_update_upstream_free(peers);
    }

    if (backup != NULL) {
        ngx_http_dynamic_update_upstream_free(backup);
    }

    uscf->peer.data = tmp_peers;

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_dynamic_update_upstream_add_peer(ngx_cycle_t *cycle,
        ngx_http_upstream_srv_conf_t *uscf, ngx_http_upstream_server_t *us)
{
    ngx_uint_t                     i, n, w, m;
    ngx_http_upstream_rr_peers_t  *peers=NULL;

    peers = uscf->peer.data;

    if (us->naddrs) {

        n = us->naddrs + peers->number;
        w = us->naddrs * us->weight + peers->total_weight;

        if (n == 0) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                    "no servers to add in upstream_add_server \"%V\" in %s:%ui",
                    &uscf->host, uscf->file_name, uscf->line);
            return NGX_ERROR;
        }

        m = peers->number;

        peers->single = (n == 1);
        peers->number = n;
        peers->weighted = (w != n);
        peers->total_weight = w;

        for (i = 0; i < us->naddrs; i++) {
            if (us->backup) {
                continue;
            }

            peers->peer[m].sockaddr = us->addrs[i].sockaddr;
            peers->peer[m].socklen = us->addrs[i].socklen;
            peers->peer[m].name = us->addrs[i].name;
            peers->peer[m].max_fails = us->max_fails;
            peers->peer[m].fail_timeout = us->fail_timeout;
            peers->peer[m].down = us->down;
            peers->peer[m].weight = us->weight;
            peers->peer[m].effective_weight = us->weight;
            peers->peer[m].current_weight = 0;

#if (NGX_HTTP_UPSTREAM_CHECK) 
            ngx_uint_t index = ngx_http_upstream_check_add_dynamic_peer(cycle->pool, uscf, &us->addrs[i]);
            peers->peer[m].check_index = index;
#endif

            m++;
        }
    }

    if (!update_flag) {
        update_flag = 1;
    }

    return NGX_OK;
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
    ngx_http_upstream_srv_conf_t            *uscf;
    ngx_http_upstream_server_t               us;
    ngx_http_dynamic_update_upstream_ctx_t  *ctx;

    struct sockaddr_in  *sin;

    uscf = conf_server->uscf;

    ctx = &conf_server->ctx;

    ngx_memzero(&us, sizeof(ngx_http_upstream_server_t));

    pool = ngx_create_pool(ngx_pagesize, cycle->log);
    if (pool == NULL) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                "upstream_del_server: no enough memory");
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
                    "upstream_del_server: has no port in %s", p);
            j--;
            continue;
        }

        n = ngx_atoi(port + 1, last - port - 1);
        if (n < 1 || n > 65535) {
            ngx_log_error(NGX_LOG_ERR, cycle->log, 0, 
                    "upstream_del_server: invalid port in %s", p);
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
                    "upstream_del_server: invalid ip in %s", p);
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
    us.weight = 0;
    us.max_fails = 0;
    us.fail_timeout = 0;

    if (us.naddrs > 0) {
        if (ngx_http_dynamic_update_upstream_del_peer(cycle, uscf, 
                    &us, conf_server) 
                != NGX_OK) 
        {
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
        ngx_http_upstream_srv_conf_t *uscf, ngx_http_upstream_server_t *us,
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    ngx_uint_t                                   i, j, n, w, len;
    ngx_flag_t                                   update_label;
    ngx_http_upstream_rr_peers_t                *peers=NULL;
    ngx_http_upstream_rr_peers_t                *tmp_peers=NULL, *tmp_backup=NULL;

    update_label = conf_server->update_label;

    len = sizeof(struct sockaddr);

    if (uscf->peer.data != NULL) {
        tmp_peers = (ngx_http_upstream_rr_peers_t *)uscf->peer.data;
        tmp_backup = tmp_peers->next;
    }

    if (tmp_peers->number == us->naddrs) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                "upstream %V backend cannt be null, cannt delete all", &uscf->host);

        return NGX_OK;
    }

    if (tmp_peers) {
        n = 0;
        w = 0;

        if (tmp_peers->number < us->naddrs) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                    "no servers to del in upstream_del_server \"%V\" in %s:%ui",
                    &uscf->host, uscf->file_name, uscf->line);
            goto invalid;
        }

        n = tmp_peers->number - us->naddrs;
        w = tmp_peers->total_weight - us->naddrs * us->weight;

        peers = ngx_calloc(sizeof(ngx_http_upstream_rr_peers_t)
                + sizeof(ngx_http_upstream_rr_peer_t) * (n - 1), cycle->log);

        if (peers == NULL) {
            goto invalid;
        }

        peers->single = (n == 1);
        peers->number = n;
        peers->weighted = (w != n);
        peers->total_weight = w;
        peers->name = &uscf->host;

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

                    tmp_peers->peer[i].check_index = NGX_MAX_VALUE;
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

        uscf->peer.data = peers;

        /* backup servers */

        peers->next = tmp_backup;

        if (update_label) {
            ngx_http_dynamic_update_upstream_event_init(tmp_peers, conf_server, NGX_DEL);
        }

        return NGX_OK;
    }

    return NGX_OK;

invalid:
    ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
            "upstream_del_server del failed \"%V\" in %s:%ui",
            &uscf->host, uscf->file_name, uscf->line);

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
ngx_http_dynamic_update_upstream_parse_json(u_char *buf, 
        ngx_http_dynamic_update_upstream_server_t *conf_server)
{
    u_char                                  *p;
    ngx_str_t                                src, dst;
    ngx_http_update_conf_t                  *upstream_conf = NULL;
    ngx_http_dynamic_update_upstream_ctx_t  *ctx;

    ctx = &conf_server->ctx;
    ctx->upstream_count = 0;

    src.len = 0, src.data = NULL;
    dst.len = 0, dst.data = NULL;

    cJSON *root = cJSON_Parse((char *)buf);
    if (root == NULL) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "dynamic_update_upstream_parse_json root error");

        return NGX_ERROR;
    }

    ctx->upstream_count++;
    if (ngx_array_init(&ctx->upstream_conf, ctx->pool, 16,
                sizeof(*upstream_conf)) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                "dynamic_update_upstream_init_upstream_conf alloc error");

        return NGX_ERROR;
    }

    cJSON *server_next;
    for (server_next = root->child; server_next != NULL; 
            server_next = server_next->next) {

        cJSON *temp1 = cJSON_GetObjectItem(server_next, "Key");
        if (temp1 != NULL && temp1->valuestring != NULL) {

            p = (u_char *)ngx_strchr(temp1->valuestring, '/');
            p = (u_char *)ngx_strchr(p + 1, '/');

            upstream_conf = ngx_array_push(&ctx->upstream_conf);

            ngx_memzero(upstream_conf, sizeof(*upstream_conf));
            ngx_sprintf(upstream_conf->sockaddr, "%*s", ngx_strlen(p + 1), p + 1);
        }
        temp1 = NULL;

        temp1 = cJSON_GetObjectItem(server_next, "Value");
        if (temp1 != NULL && temp1->valuestring != NULL) {

            src.data = (u_char *)temp1->valuestring;
            src.len = ngx_strlen(temp1->valuestring);

            dst.data = ngx_pcalloc(ctx->pool, 1024);
            dst.len = 0;

            ngx_decode_base64(&dst, &src);
        }
        temp1 = NULL;

        upstream_conf->weight = 0;
        upstream_conf->max_fails = 0;
        upstream_conf->fail_timeout = 0;

        upstream_conf->down = 0;
        upstream_conf->backup = 0;

        p = NULL;

        if (dst.data != NULL && dst.len != 0) {

            p = dst.data;

            cJSON *sub_root = cJSON_Parse((char *)p);
            if (sub_root == NULL) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                    "dynamic_update_upstream_parse_json root error");

                continue;
            }

            cJSON *temp1 = cJSON_GetObjectItem(sub_root, "weight");
            if (temp1 != NULL && temp1->valuestring != NULL) {

                upstream_conf->weight = ngx_atoi((u_char *)temp1->valuestring, 
                                                (size_t)ngx_strlen(temp1->valuestring));
            }
            temp1 = NULL;

            temp1 = cJSON_GetObjectItem(sub_root, "max_fails");
            if (temp1 != NULL && temp1->valuestring != NULL) {

                upstream_conf->max_fails = ngx_atoi((u_char *)temp1->valuestring, 
                                                (size_t)ngx_strlen(temp1->valuestring));
            }
            temp1 = NULL;

            temp1 = cJSON_GetObjectItem(sub_root, "fail_timeout");
            if (temp1 != NULL && temp1->valuestring != NULL) {

                upstream_conf->fail_timeout = ngx_atoi((u_char *)temp1->valuestring, 
                                                (size_t)ngx_strlen(temp1->valuestring));
            }
            temp1 = NULL;

            temp1 = cJSON_GetObjectItem(sub_root, "down");
            if (temp1 != NULL && temp1->valuestring != NULL) {

                upstream_conf->down = ngx_atoi((u_char *)temp1->valuestring, 
                                                (size_t)ngx_strlen(temp1->valuestring));
            }
            temp1 = NULL;

            temp1 = cJSON_GetObjectItem(sub_root, "backup");
            if (temp1 != NULL && temp1->valuestring != NULL) {

                upstream_conf->backup = ngx_atoi((u_char *)temp1->valuestring, 
                                                (size_t)ngx_strlen(temp1->valuestring));
            }
            temp1 = NULL;

            cJSON_Delete(sub_root);
        }
    }
    cJSON_Delete(root);

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

    duscf->consul_host.len = 0;
    duscf->consul_host.data = NULL;

    duscf->update_timeout = NGX_CONF_UNSET_MSEC;
    duscf->update_interval = NGX_CONF_UNSET_MSEC;

    duscf->delay_delete = NGX_CONF_UNSET_MSEC;

    duscf->strong_dependency = NGX_CONF_UNSET_UINT;

    ngx_memzero(&duscf->us, sizeof(duscf->us));

    return duscf;
}


static char *
ngx_http_dynamic_update_upstream_init_srv_conf(ngx_conf_t *cf, void *conf, ngx_uint_t num)
{
    ngx_http_upstream_srv_conf_t                *uscf = conf;
    ngx_http_dynamic_update_upstream_server_t   *conf_server;
    ngx_http_dynamic_update_upstream_srv_conf_t *duscf;

    if (uscf->srv_conf == NULL) {
        return NGX_CONF_OK;
    }

    duscf = ngx_http_conf_upstream_srv_conf(uscf, ngx_http_dynamic_update_upstream_module);

    if (duscf->consul_host.data == NULL && duscf->consul_host.len == 0) {
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

    conf_server->index = 0;
    conf_server->update_label = 0;

    conf_server->conf = duscf;
    conf_server->uscf = uscf;

    conf_server->host.len = uscf->host.len;
    conf_server->host.data = uscf->host.data;

    return NGX_CONF_OK;
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

                break;
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

        if (!conf_server[i].update_label && update_flag) {
            conf_server[i].update_label = 1;
            update_flag = 0;
        }
    }

    ngx_http_dynamic_update_upstream_add_timers(cycle);

    return NGX_OK;
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
            "http update upstream add_timers, shm_name: %V",
            peer->pc.name);

    srandom(ngx_pid);
    for (i = 0; i < dynamic_upstream_ctx->upstream_num; i++) {

        ngx_queue_init(&peer[i].add_ev);
        ngx_queue_init(&peer[i].delete_ev);

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
                "ngx_http_dynamic_update_upstream_begin_handler wrong");
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

    if (ngx_http_dynamic_update_upstream_init_peer(event) != NGX_OK) {
        return;
    }

    peer = event->data;
    conf = peer->conf;
    ngx_add_timer(&peer->update_timeout_ev, conf->update_timeout);

    rc = ngx_event_connect_peer(&peer->pc);

    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        ngx_log_error(NGX_LOG_ERR, event->log, 0,
                "update upstream cant connect with peer: %V ",
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
            "dynamic update upstream send error with peer: %V", peer->pc.name);

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
            "dynamic_update_upstream_recv: recv error with peer: %V",
            peer->pc.name);

    ngx_http_dynamic_update_upstream_clean_event(peer);
}


static ngx_int_t
ngx_http_dynamic_update_upstream_init_peer(ngx_event_t *event)
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
                        "dynamic_update_upstream_begin: recv no enough memory");
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

        delay_event->delay_delete_ev.handler =
            ngx_http_dynamic_update_upstream_del_delay_delete;
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
    ngx_http_upstream_rr_peers_t  *tmp_peers=NULL, *tmp_backup=NULL;

    delay_event = event->data;

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

    tmp_peers = delay_event->data;
    tmp_backup = tmp_peers->next;

    if (tmp_peers != NULL) {

        ngx_free(tmp_peers);
        tmp_peers = NULL;
    }

    if (tmp_backup && tmp_backup->number > 0) {
 
        ngx_free(tmp_backup);
        tmp_backup = NULL;
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

            if (tmp_peers->peer[i].check_index != NGX_MAX_VALUE) {
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

    ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
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
                "ngx_http_dynamic_update_upstream_get_all: http client create error");
        return NGX_ERROR;
    }

    ngx_int_t status = ngx_http_client_conn(client);
    if (status != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "ngx_http_dynamic_update_upstream_get_all: http client conn error");

        ngx_http_client_destroy(client);
        return NGX_ERROR;
    }

    char *response = NULL;

    ngx_http_client_send(client, conf_server);

    if (ngx_http_client_recv(client, &response, 0) <= 0) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "ngx_http_dynamic_update_upstream_get_all: http client recv fail");

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
                "conf_client_create: setsockopt SO_SNDTIMEO error");
        ngx_http_client_destroy(client);
        return NULL;
    }

    if (setsockopt(client->sd, SOL_SOCKET, SO_RCVTIMEO, (void *) &tv_timeout, sizeof(struct timeval)) < 0) {
        ngx_log_error(NGX_LOG_ERR, cycle->log, 0,
                "conf_client_create: setsockopt SO_RCVTIMEO error");
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

    b = ngx_create_temp_buf(r->pool, ngx_pagesize);
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
