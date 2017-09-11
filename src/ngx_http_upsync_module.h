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

#define ngx_strtoull(nptr, endptr, base) strtoull((const char *) nptr, \
                                                  (char **) endptr, (int) base)

#define NGX_INDEX_HEADER "X-Consul-Index"
#define NGX_INDEX_HEADER_LEN 14

#define NGX_INDEX_ETCD_HEADER "X-Etcd-Index"
#define NGX_INDEX_ETCD_HEADER_LEN 12

#define NGX_MAX_HEADERS 20
#define NGX_MAX_ELEMENT_SIZE 512

#define NGX_DELAY_DELETE 30 * 60 * 1000   //75 * 1000

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


static int ngx_libc_cdecl ngx_http_upsync_chash_cmp_points(const void *one, 
    const void *two);
static ngx_int_t ngx_http_upsync_chash_init(ngx_http_upstream_srv_conf_t *uscf,
    ngx_http_upstream_rr_peers_t *tmp_peers);
static ngx_int_t ngx_http_upsync_del_chash_peer(
    ngx_http_upstream_srv_conf_t *uscf);


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


static ngx_int_t
ngx_http_upsync_chash_init(ngx_http_upstream_srv_conf_t *uscf,
    ngx_http_upstream_rr_peers_t *tmp_peers)
{
    size_t                                    new_size;
    size_t                                    host_len, port_len;
    u_char                                   *host, *port, c;
    uint32_t                                  hash, base_hash;
    ngx_str_t                                *server;
    ngx_uint_t                                npoints, new_npoints;
    ngx_uint_t                                i, j;
    ngx_http_upstream_rr_peer_t              *peer;
    ngx_http_upstream_rr_peers_t             *peers;
    ngx_http_upstream_chash_points_t         *points;
    ngx_http_upstream_hash_srv_conf_t        *hcf;
    union {
        uint32_t                              value;
        u_char                                byte[4];
    } prev_hash;

    hcf = ngx_http_conf_upstream_srv_conf(uscf, ngx_http_upstream_hash_module);
    if(hcf->points == NULL) {
        return 0;
    }

    peers = uscf->peer.data;
    if (tmp_peers != NULL) {
        new_npoints = peers->total_weight * 160;

        new_size = sizeof(ngx_http_upstream_chash_points_t)
                   + sizeof(ngx_http_upstream_chash_point_t) * (new_npoints - 1);

        points = ngx_calloc(new_size, ngx_cycle->log);
        if (points == NULL ) {
            return NGX_ERROR;
        }
        ngx_free(hcf->points); /* free old points */
        hcf->points = points;

        for (peer = peers->peer; peer; peer = peer->next) {
            server = &peer->server;

            /*
            * Hash expression is compatible with Cache::Memcached::Fast:
            * crc32(HOST \0 PORT PREV_HASH).
            */

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

            prev_hash.value = 0;
            npoints = peer->weight * 160;

            for (j = 0; j < npoints; j++) {
                hash = base_hash;

                ngx_crc32_update(&hash, prev_hash.byte, 4);
                ngx_crc32_final(hash);

                points->point[points->number].hash = hash;
                points->point[points->number].server = server;
                points->number++;

#if (NGX_HAVE_LITTLE_ENDIAN)
                prev_hash.value = hash;
#else
                prev_hash.byte[0] = (u_char) (hash & 0xff);
                prev_hash.byte[1] = (u_char) ((hash >> 8) & 0xff);
                prev_hash.byte[2] = (u_char) ((hash >> 16) & 0xff);
                prev_hash.byte[3] = (u_char) ((hash >> 24) & 0xff);
#endif
            }
        }

    } else {
        new_npoints = peers->total_weight * 160;

        new_size = sizeof(ngx_http_upstream_chash_points_t)
                   + sizeof(ngx_http_upstream_chash_point_t) * (new_npoints - 1);

        points = ngx_calloc(new_size, ngx_cycle->log);
        if (points == NULL ) {
            return NGX_ERROR;
        }

        ngx_memcpy(points, hcf->points, new_size);
        ngx_pfree(ngx_cycle->pool, hcf->points);

        hcf->points = points;

        return NGX_OK;
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


static ngx_int_t
ngx_http_upsync_del_chash_peer(ngx_http_upstream_srv_conf_t *uscf)
{
    size_t                                    host_len, port_len;
    u_char                                   *host, *port, c;
    uint32_t                                  hash, base_hash;
    ngx_str_t                                *server;
    ngx_uint_t                                npoints, i, j;
    ngx_http_upstream_rr_peer_t              *peer;
    ngx_http_upstream_rr_peers_t             *peers;
    ngx_http_upstream_chash_points_t         *points;
    ngx_http_upstream_hash_srv_conf_t        *hcf;    
    union {
        uint32_t                              value;
        u_char                                byte[4];
    } prev_hash;

    hcf = ngx_http_conf_upstream_srv_conf(uscf, ngx_http_upstream_hash_module);
    if(hcf->points == NULL) {
        return 0;    
    }

    peers = uscf->peer.data;

    points = hcf->points;
    points->number = 0;

    for (peer = peers->peer; peer; peer = peer->next) {
        server = &peer->server;

        /*
         * Hash expression is compatible with Cache::Memcached::Fast:
         * crc32(HOST \0 PORT PREV_HASH).
         */

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

        prev_hash.value = 0;
        npoints = peer->weight * 160;

        for (j = 0; j < npoints; j++) {
            hash = base_hash;

            ngx_crc32_update(&hash, prev_hash.byte, 4);
            ngx_crc32_final(hash);

            points->point[points->number].hash = hash;
            points->point[points->number].server = server;
            points->number++;

#if (NGX_HAVE_LITTLE_ENDIAN)
            prev_hash.value = hash;
#else
            prev_hash.byte[0] = (u_char) (hash & 0xff);
            prev_hash.byte[1] = (u_char) ((hash >> 8) & 0xff);
            prev_hash.byte[2] = (u_char) ((hash >> 16) & 0xff);
            prev_hash.byte[3] = (u_char) ((hash >> 24) & 0xff);
#endif
        }
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
