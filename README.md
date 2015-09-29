Name
====

nginx-upsync-module - Nginx C module, nginx + consul server discovery service

Table of Contents
=================

* [Name](#name)
* [Status](#status)
* [Synopsis](#synopsis)
* [Description](#description)
* [Directives](#functions)
    * [consul](#consul)
    * [update_interval](#update_interval)
    * [update_timeout](#update_timeout)
    * [strong_dependency](#strong_dependency)
    * [upstream_show](#upstream_show)
* [Consul_interface](#consul_interface)
* [TODO](#todo)
* [Compatibility](#compatibility)
* [Installation](#installation)
* [Author](#author)
* [Copyright and License](#copyright-and-license)
* [See Also](#see-also)
* [Source Dependency](#source-dependency)

Status
======

This module is still under active development and is considered production ready.

Synopsis
========

```nginx
http {
    upstream test {
        # fake server otherwise ngx_http_upstream will report error when startup
        server 127.0.0.1:11111;

        # all backend server will pull from consul when startup and will delete fake server
        consul 127.0.0.1:8500/v1/kv/upstreams/test update_timeout=6m update_interval=3s strong_dependency=off;
    }

    upstream bar {
        server 127.0.0.1:8090 weight=1, fail_timeout=10, max_fails=3;
    }

    server {
        listen 8080;

        location = /proxy_test {
            proxy_pass http://test;
        }

        location = /bar {
            proxy_pass http://bar;
        }

        location = /upstream_show {
            upstream_show;
        }

    }
}
```

Description
======

This module provides a method to discover backend servers. Supporting dynamicly adding or deleting backend server through consul, module will timely pull new backend server list from consul to update nginx ip router. Nginx needn't reload. Having some advantages than others:

* timely

      module send key to consul with index, consul will compare it with its index, if index doesn't change connection will hang five minutes, in the period any operation to the key-value, will feed back rightaway.

* performance

      Pulling from consul equal a request to nginx, updating ip router nginx needn't reload, so affecting nginx performance is little.

* stability

      Even if one pulling failed, it will pull next update_interval, so guaranteing backend server stably provides service.

* health_check

      nginx-upsync-module support adding or deleting servers health check, needing nginx_upstream_check_module. Recommending nginx-upsync-module + nginx_upstream_check_module.

Diretives
======

consul
-----------
```
syntax: consul $consul.api.com:$port/v1/kv/upstreams/$upstream_name [update_interval=second/minutes] [update_timeout=second/minutes] [strong_dependency=off/on]
```
default: none, if parameters omitted, default parameters are update_interval=5s update_timeout=6m strong_dependency=off

context: upstream

description: Pull upstream servers from consul.

The parameters' meanings are:

* update_interval

    pulling servers from consul interval time.

* update_timeout

    pulling servers from consul request timeout.

* strong_dependency

    when nginx start up if depending on consul, and consul is not working, nginx will boot failed, otherwise booting normally.

[Back to TOC](#table-of-contents)       

upstream_show
-----------
`syntax: upstream_show`

default: none

context: upstream

description: Show specific upstream all backend servers.

```configure
     location /upstream_list {
         upstream_show;
     }
```

```request
curl http://127.0.0.1:8500/upstream_list?test;
```

[Back to TOC](#table-of-contents)       

Consul_interface
======

you can add or delete backend server through consul_ui or http_interface.

http_interface example:

* add
```
    curl -X PUT http://$consul_ip:$port/v1/kv/upstreams/$upstream_name/$backend_ip:$backend_port
```
    default: weight=1 max_fails=2 fail_timeout=10 down=0 backup=0;

```
    curl -X PUT -d '{"weight":"1", "max_fails":"2", "fail_timeout":"10s"}' http://$consul_ip:$port/v1/kv/upstreams/$upstream_name/$backend_ip:$backend_port
```
    value support json format.

* delete
```
    curl -X DELETE http://$consul_ip:$port/v1/kv/upstreams/$upstream_name/$backend_ip:$backend_port
```

* check
```
    curl http://$consul_ip:$port/v1/kv/upstreams/$upstream_name?recurse
```

[Back to TOC](#table-of-contents)       

TODO
====

* support least_conn load_balancing

[Back to TOC](#table-of-contents)

Compatibility
=============

The module was developed base on nginx1.8.0, but it is not embeded to the request process, so it can be used in any version of nginx theoretically. To be veryfied.

[Back to TOC](#table-of-contents)

Installation
============

This module can be used independently, can be download[Github](https://github.com/weibocom/nginx-upsync-module.git).

Grab the nginx source code from [nginx.org](http://nginx.org/), for example, the version 1.8.0 (see nginx compatibility), and then build the source with this module:

```bash
wget 'http://nginx.org/download/nginx-1.8.0.tar.gz'
tar -xzvf nginx-1.8.0.tar.gz
cd nginx-1.8.0/
```

```bash
./configure --add-module=/path/to/nginx-upsync-module
make
make install
```

if you support nginx-upstream-check-module
```bash
./configure --add-module=/path/to/nginx-upstream-check-module --add-module=/path/to/nginx-upsync-module
make
make install
```

[Back to TOC](#table-of-contents)

Author
======

Xiaokai Wang (王晓开) <xiaokai.wang@live.com>, Weibo Inc.

[Back to TOC](#table-of-contents)

Copyright and License
=====================

This README template copy from agentzh.

This module is licensed under the BSD license.

Copyright (C) 2014 by Xiaokai Wang <xiaokai.wang@live.com></xiaokai.wang@live.com>

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

[Back to TOC](#table-of-contents)

see also
========
* the nginx_upstream_check_module: https://github.com/alibaba/tengine/blob/master/src/http/ngx_http_upstream_check_module.c
* the nginx_upstream_check_module patch: https://github.com/yaoweibin/nginx_upstream_check_module
* the all project as a service base on docker:

[back to toc](#table-of-contents)

source dependency
========
* Cjson: https://github.com/kbranigan/cJSON
* http-parser: https://github.com/joyent/http-parser

[back to toc](#table-of-contents)


