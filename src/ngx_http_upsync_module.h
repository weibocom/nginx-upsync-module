#ifndef _NGX_HTTP_UPSYNC_MODELE_H_INCLUDED_
#define _NGX_HTTP_UPSYNC_MODELE_H_INCLUDED_


#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_config.h>

#include "ngx_http_json.h"
#include "ngx_http_parser.h"


#define ngx_strrchr(s1, c)              strrchr((const char *) s1, (int) c)
#define ngx_ftruncate(fd, offset)       ftruncate(fd, offset)
#define ngx_lseek(fd, offset, whence)   lseek(fd, offset, whence)
#define ngx_fgets(fp, offset, whence)   fgets(fp, offset, whence)
#define ngx_fopen(path, mode)           fopen(path, mode)
#define ngx_fclose(fp)                  fclose(fp)


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

#define NGX_HTTP_LB_DEFAULT        0
#define NGX_HTTP_LB_ROUNDROBIN     1
#define NGX_HTTP_LB_IP_HASH        2
#define NGX_HTTP_LB_LEAST_CONN     4
#define NGX_HTTP_LB_HASH_MODULA    8
#define NGX_HTTP_LB_HASH_KETAMA    16

#if (NGX_HTTP_UPSTREAM_CHECK) 

extern ngx_uint_t ngx_http_upstream_check_add_dynamic_peer(ngx_pool_t *pool,
    ngx_http_upstream_srv_conf_t *uscf, ngx_addr_t *peer_addr);
extern void ngx_http_upstream_check_delete_dynamic_peer(ngx_str_t *name,
    ngx_addr_t *peer_addr);

#endif


/*****************************least_conn****************************/

extern ngx_module_t ngx_http_upstream_least_conn_module;


typedef struct {
    ngx_uint_t                        *conns;
} ngx_http_upstream_least_conn_conf_t;

/*****************************least_conn_end*************************/


/******************************hash*********************************/

extern  ngx_module_t ngx_http_upstream_hash_module;


typedef struct {
    uint32_t                            hash;
    ngx_str_t                          *server;
} ngx_http_upstream_chash_point_t;


typedef struct {
    ngx_uint_t                          number;
    ngx_http_upstream_chash_point_t     point[1];
} ngx_http_upstream_chash_points_t;


typedef struct {
    ngx_http_complex_value_t            key;
    ngx_http_upstream_chash_points_t   *points;
} ngx_http_upstream_hash_srv_conf_t;

/****************************hash_end*******************************/


static ngx_int_t ngx_http_upsync_least_conn_init(
    ngx_http_upstream_srv_conf_t *uscf, ngx_uint_t peers_old_count);
static ngx_int_t ngx_http_upsync_del_peer_least_conn(
    ngx_http_upstream_srv_conf_t *uscf, ngx_uint_t pos);

static int ngx_libc_cdecl ngx_http_upsync_chash_cmp_points(const void *one, 
    const void *two);
static void ngx_http_upsync_chash(ngx_http_upstream_rr_peer_t *peer, 
    ngx_http_upstream_chash_points_t *points);
static ngx_int_t ngx_http_upsync_chash_init(ngx_http_upstream_srv_conf_t *uscf,
    ngx_uint_t new_peer_count);
static ngx_int_t ngx_http_upsync_del_chash_peer(
    ngx_http_upstream_srv_conf_t *uscf);


static ngx_int_t 
ngx_http_upsync_least_conn_init(ngx_http_upstream_srv_conf_t *uscf, 
    ngx_uint_t peers_old_count)
{
    ngx_uint_t                            *conns, n;
    ngx_http_upstream_rr_peers_t          *peers;
    ngx_http_upstream_least_conn_conf_t   *lcf;


    lcf = ngx_http_conf_upstream_srv_conf(uscf,
                                          ngx_http_upstream_least_conn_module);
    if(lcf->conns == NULL) {
        return 0;   
    }

    peers = uscf->peer.data;
    n = peers->number;
    n += peers->next ? peers->next->number : 0;

    conns = ngx_calloc(n, ngx_cycle->log);
    if (conns == NULL ) {
        return NGX_ERROR;
    }

    if (peers_old_count == 0) {
        ngx_memcpy(conns, lcf->conns, peers->number);
        ngx_pfree(ngx_cycle->pool, lcf->conns);

    } else {

        ngx_memcpy(conns, lcf->conns, peers_old_count);
        ngx_free(lcf->conns);
    }

    lcf->conns = conns;
   
    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_del_peer_least_conn(ngx_http_upstream_srv_conf_t *uscf, 
    ngx_uint_t pos)
{
    ngx_uint_t                               i;
    ngx_http_upstream_rr_peers_t            *peers;
    ngx_http_upstream_least_conn_conf_t     *lcf;

    peers = uscf->peer.data;

    lcf = ngx_http_conf_upstream_srv_conf(uscf,
                                          ngx_http_upstream_least_conn_module);

    if(lcf->conns == NULL) {
        return NGX_OK;
    }

    for (i = pos; i < peers->number; i++) {
        lcf->conns[i] = lcf->conns[i + 1];
    }

    return NGX_OK;
}


static int ngx_libc_cdecl
ngx_http_upsync_chash_cmp_points(const void *one, const void *two)
{
    ngx_http_upstream_chash_point_t *first =
                                       (ngx_http_upstream_chash_point_t *) one;
    ngx_http_upstream_chash_point_t *second =
                                       (ngx_http_upstream_chash_point_t *) two;

    if (first->hash < second->hash) {
        return -1;

    } else if (first->hash > second->hash) {
        return 1;

    } else {
        return 0;
    }
}


static void
ngx_http_upsync_chash(ngx_http_upstream_rr_peer_t *peer, 
    ngx_http_upstream_chash_points_t *points)
{
    size_t                                 host_len, port_len;
    u_char                                *host, *port, c;
    uint32_t                               hash, base_hash, pre_hash;
    ngx_str_t                             *server;
    ngx_uint_t                             npoints, j;

    server = &peer->server;
    if (server->len >= 5
        && ngx_strncasecmp(server->data, (u_char *) "unix:", 5) == 0)
    {
        host = server->data + 5;
        host_len = server->len - 5;
        port = NULL;
        port_len = 0;
        goto done;
    }

    for (j = 0; j < server->len; j++) {
        c = server->data[server->len - j - 1];

        if (c == ':') {
            host = server->data;
            host_len = server->len - j - 1;
            port = server->data + server->len - j;
            port_len = j;
            goto done;
        }

        if (c < '0' || c > '9') {
            break;
        }
    }

    host = server->data;
    host_len = server->len;
    port = NULL;
    port_len = 0;

    done:

        ngx_crc32_init(base_hash);
        ngx_crc32_update(&base_hash, host, host_len);
        ngx_crc32_update(&base_hash, (u_char *) "", 1);
        ngx_crc32_update(&base_hash, port, port_len);

        pre_hash = 0;
        npoints = peer->weight * 160;

        for(j = 0; j < npoints; j++) {
            hash = base_hash;

            ngx_crc32_update(&hash, (u_char *)&pre_hash, sizeof(uint32_t));
            ngx_crc32_final(hash);

            points->point[points->number].hash = hash;
            points->point[points->number].server = server;
            points->number++;

            pre_hash = hash;
        }
}


static ngx_int_t
ngx_http_upsync_chash_init(ngx_http_upstream_srv_conf_t *uscf,
    ngx_uint_t new_peer_count)
{
    size_t                                    old_size, new_size;
    ngx_uint_t                                npoints, i, j;
    ngx_http_upstream_rr_peer_t              *peer;
    ngx_http_upstream_rr_peers_t             *peers;
    ngx_http_upstream_chash_points_t         *points;
    ngx_http_upstream_hash_srv_conf_t        *hcf;

    hcf = ngx_http_conf_upstream_srv_conf(uscf, ngx_http_upstream_hash_module);
    if(hcf->points == NULL) {
        return 0;    
    }

    peers = uscf->peer.data;

    if (new_peer_count != 0) {
        npoints = (peers->total_weight - peers->peer[peers->number - 1].weight) * 160;

        old_size = sizeof(ngx_http_upstream_chash_points_t)
                   + sizeof(ngx_http_upstream_chash_point_t) * (npoints - 1);
        new_size = old_size
                   + sizeof(ngx_http_upstream_chash_point_t) * peers->peer[peers->number - 1].weight * 160;

        points = ngx_calloc(new_size, ngx_cycle->log);
        if (points == NULL ) {
            return NGX_ERROR;
        }

        ngx_memcpy(points, hcf->points, old_size);
        ngx_free(hcf->points);

    } else {
        npoints = peers->total_weight * 160;

        new_size = sizeof(ngx_http_upstream_chash_points_t)
                   + sizeof(ngx_http_upstream_chash_point_t) * (npoints - 1);

        points = ngx_calloc(new_size, ngx_cycle->log);
        if (points == NULL ) {
            return NGX_ERROR;
        }

        ngx_memcpy(points, hcf->points, new_size);
        ngx_pfree(ngx_cycle->pool, hcf->points);

        return NGX_OK;
    }

    hcf->points = points;
    peer = &peers->peer[peers->number - 1];

    ngx_http_upsync_chash(peer, points);

    ngx_qsort(points->point,
              points->number,
              sizeof(ngx_http_upstream_chash_point_t),
              ngx_http_upsync_chash_cmp_points);

    for (i = 0, j = 1; j < points->number; j++) {
        if (points->point[i].hash != points->point[j].hash) {
                points->point[++i] = points->point[j];
        }
    }

    points->number = i + 1;
    
    return NGX_OK;
}


static ngx_int_t
ngx_http_upsync_del_chash_peer(ngx_http_upstream_srv_conf_t *uscf)
{
    ngx_uint_t                                i, j;
    ngx_http_upstream_rr_peer_t              *peer;
    ngx_http_upstream_rr_peers_t             *peers;
    ngx_http_upstream_chash_points_t         *points;
    ngx_http_upstream_hash_srv_conf_t        *hcf;    

    hcf = ngx_http_conf_upstream_srv_conf(uscf, ngx_http_upstream_hash_module);
    if(hcf->points == NULL) {
        return 0;    
    }

    peers = uscf->peer.data;

    points = hcf->points;
    points->number = 0;
  
    for (i = 0; i < peers->number; i++) {
        peer = &peers->peer[i];
        ngx_http_upsync_chash(peer, points);

    }

    ngx_qsort(points->point,
              points->number,
              sizeof(ngx_http_upstream_chash_point_t),
              ngx_http_upsync_chash_cmp_points);

    for (i = 0, j = 1; j < points->number; j++) {
        if (points->point[i].hash != points->point[j].hash) {
            points->point[++i] = points->point[j];
        }
    }

    points->number = i + 1;

    return NGX_OK;
}


#endif //_NGX_HTTP_UPSYNC_MODELE_H_INCLUDED_
