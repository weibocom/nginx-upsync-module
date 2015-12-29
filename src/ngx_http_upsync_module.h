#ifndef _NGX_HTTP_UPSYNC_MODELE_H_INCLUDED_
#define _NGX_HTTP_UPSYNC_MODELE_H_INCLUDED_


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


#if (NGX_HTTP_UPSTREAM_CHECK) 

extern ngx_uint_t ngx_http_upstream_check_add_dynamic_peer(ngx_pool_t *pool,
    ngx_http_upstream_srv_conf_t *us, ngx_addr_t *peer_addr);
extern void ngx_http_upstream_check_delete_dynamic_peer(ngx_str_t *name,
    ngx_addr_t *peer_addr);

#endif


#endif //_NGX_HTTP_UPSYNC_MODELE_H_INCLUDED_
