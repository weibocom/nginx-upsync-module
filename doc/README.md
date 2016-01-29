nginx-upsync-module
====

目录
=================

* [模块背景](#模块背景)
* [方案设计](#方案设计)
    * [业界方式](#业界方式)
    * [开发方案设计](#开发方案设计)
        * [http_api方案](#http_api方案)
        * [upsync方案](#upsync方案)
* [方案实现](#方案实现)
    * [列表更新方式](#列表更新方式)
    * [高可用性](#高可用性)
    * [兼容性](#兼容性)
* [性能测试](#性能测试)
* [应用案例](#应用案例)

模块背景
======

针对模块的开发背景，以微博的业务场景为例，每年的元旦、春晚、红包飞会带来巨大的流量挑战，这些业务场景的主要特点是：瞬间峰值高，互动时间短。基本上一次峰值事件，互动时间都会3小时以内，而明星突发新闻事件，红包飞这种业务，经常会遇到高达多倍的瞬间峰值。传统的应对手段，主要是提前申请足够的设备，保证冗余。

这么做除了成本高外，对系统进行水平扩容时，耗费的时间久，而且扩容缩容流程繁琐。对于明星的突发事件，流量突增几倍的情况下，进行扩容缩容的繁琐操作便会更加突出，而且这个时候对nginx 进行reload 是有一定危险的，之前的操作表明，在流量比较高时进行nginx 的reload 会出现服务的抖动，造成部分请求耗时增长20%-50%。

为了节约成本以及随着云服务的兴起，平台采用了混合云的部署方式，对资源进行弹性调度。对于第三方公有云服务，比如阿里云按照小时计费，所以需要按需部署，为了避免在弹性调度时，对服务造成不必要的抖动，以及自动化扩容缩容操作，开发了nginx-upsync-module，实现nginx 无损的扩容缩容。

方案设计
========

方案介绍了当前业界已经存在的，分析了其相应的优缺点，并自己设计实现了两个方案，并综合对比选择了upsync方式。

业界方式
-----------

实现nginx层的弹性扩容、缩容，当前业界已存在的、不需二次开发的有基于consul的consul-template和tengine提供的基于dns的服务发现。

#####两种方式的分析对比：

| - | dns | consul-template |
| --- | :---: | :---: |
| 实时性 | 差 | 中 |
| 容错性 | 强 | 强 |
| 一致性 |强 |强 |
| 复杂性 | 易 | 繁 |

tengine团队开发了自己的模块，该模块可以动态的解析upstream conf下的域名。这种方式操作简单，只要修改dns下挂载的server列表便可；缺点是现在的第一版负载均衡策略待完善；另一点默认解析一次的时间是30s，若配置的时间过短，可能对dns server形成压力；再一点是基于dns的服务，下面不能挂过多的server，否则会发生截断。

consul-template与consul作为一个组合，consul作为db，consul-template部署于nginx server上，consul-template定时向consul发起请求，发现value值有变化，便会更新本地的nginx相关配置文件，发起reload命令。但是在流量比较重的情况下，发起reload会对性能造成影响。reload的同时会引发新的work进程的创建，在一段时间内新旧work进程会同时存在，并且旧的work进程会频繁的遍历connection链表，查看是否请求已经处理结束，若结束便退出进程；另reload也会造成nginx与client和backend的长链接关闭，新的work进程需要创建新的链接。

reload造成的性能影响：

<img src="https://github.com/weibocom/nginx-upsync-module/raw/master/doc/images/consul-template-reload-qps.png" alt="consul-template-reload-qps" height="50%" width="60%">
```
图示：reload时nginx的请求处理能力会下降（注：nginx对于握手成功的请求不会丢失）
```
<img src="https://github.com/weibocom/nginx-upsync-module/raw/master/doc/images/consul-template-reload-cost.png" alt="consul-template-reload-cost" height="50%" width="60%">
```
图示：reload时耗时会发生波动，波动幅度甚至达50%+
```

基于上述的原因，设计并实现了另外两套方案，避免对nginx进行reload。

开发方案设计
-----------

###http_api方案

此方案提供nginx http api，添加／删除server时，通过调用api向nginx发出请求，操作简单、便利。架构图如下：

<img src="https://github.com/weibocom/nginx-upsync-module/raw/master/doc/images/nginx-http-api-arch.png" alt="nginx-http-api-arch" height="50%" width="40%">

http api除了操作简单、方便，而且实时性好；缺点是分布式一致性难于保证，如果某一条注册失败，便会造成服务配置的不一致，容错复杂；另一个就是如果扩容nginx服务器，需要重新注册server（可参考nginx-upconf-module，正在完善）。

###upsync方案

upsync方式引入了第三方组件，作为nginx的upstream server配置的db，架构图如下：

<img src="https://github.com/weibocom/nginx-upsync-module/raw/master/doc/images/nginx-upsync-arch.png" alt="nginx-upsync-arch" height="80%" width="60%">

所有的后端server列表存于consul，便于nginx横向扩展，实时拉取，容错性更好，而且可以结合db的KV服务，提高实时性。

通过上面的综合对比，选取upsync的方式，开发了nginx模块nginx-upsync-module。对于nginx配置db的选取，由于当前docker技术十分火热，选用了consul，另模块不强依赖于consul，可以横向的扩展支持etcd、zookeeper等。下面的实现基于consul进行介绍。

方案实现
======

基于upsync方式，开发了模块nginx-upsync-module，它的功能是拉取consul的后端server的列表，并更新nginx的路由信息。此模块不依赖于任何第三方模块。

列表更新方式
------------

consul 作为nginx的db，利用consul的KV服务，每个nginx work进程独立的去拉取各个upstream的配置，并更新各自的路由。流程图如下：

![list_update](https://github.com/weibocom/nginx-upsync-module/raw/master/doc/images/list_update.bmp)

每个work进程定时的去consul拉取相应upstream的配置，定时的间隔可配；其中consul提供了time_wait机制，利用value的版本号，若consul发现对应upstream的值没有变化，便会hang住这个请求5分钟（默认），在这五分钟内对此upstream的任何操作，都会立刻返回给nginx，对相应路由进行更新。对于拉取的间隔可以结合场景的需要进行配置，基本可以实现所要求的实时性。upstream变更后，除了更新nginx的缓存路由信息，还会把本upstream的后端server列表dump到本地，保持本地server信息与consul的一致性。

除了注册／注销后端的server到consul，会更新到nginx的upstream路由信息外，对后端server属性的修改也会同步到nginx的upstream路由。当前本模块支持修改的属性有weight、max_fails、fail_timeout、down，修改server的权重可以动态的调整后端的流量，若想要临时移除server，可以把server的down属性置为1（当前down的属性暂不支持dump到本地的server列表内），流量便会停止打到该server，若要恢复流量，可重新把down置为0。

另外每个work进程各自拉取、更新各自的路由表，采用这种方式的原因：一是基于nginx的进程模型，彼此间数据独立、互不干扰；二是若采用共享内存，需要提前预分配，灵活性可能受限制，而且还需要读写锁，对性能可能存在潜在的影响；三是若采用共享内存，进程间协调去拉取配置，会增加它的复杂性，拉取的稳定性也会受到影响。基于这些原因，便采用了各自拉取的方式。

高可用性
------------

nginx的后端列表更新依赖于consul，但是不强依赖于它，表现在：一是即使中途consul意外挂了，也不会影响nginx的服务，nginx会沿用最后一次更新的服务列表继续提供服务；二是若consul重新启动提供服务，这个时候nginx会继续去consul探测，这个时候consul的后端服务列表发生了变化，也会及时的更新到nginx。

另一方面，work进程每次更新都会把后端列表dump到本地，目的是降低对consul的依赖性，即使在consul不可用之时，也可以reload nginx。nginx 启动流程图如下：

![start_fllow](https://github.com/weibocom/nginx-upsync-module/raw/master/doc/images/start_fllow.bmp)

nginx启动时，master进程首先会解析本地的配置文件，解析完成功，接着进行一系列的初始化，之后便会开始work进程的初始化。work初始化时会去consul拉取配置，进行work进程upstream路由信息的更新，若拉取成功，便直接更新，若拉取失败，便会打开配置的dump后端列表的文件，提取之前dump下来的server信息，进行upstream路由的更新，之后便开始正常的提供服务。

每次去拉取consul都会设置连接超时，由于consul在无更新的情况下默认会hang五分钟，所以响应超时配置时间应大于五分钟。大于五分钟之后，consul依旧没有返回，便直接做超时处理。

兼容性
------------

整体上讲本模块只是更新后端的upstream路由信息，不嵌入其它模块，同时也不影响其它模块的功能，亦不会影响nginx-1.9.9的几种负载均衡算法：least_conn、hash_ip等。

除此之外，模块天然支持健康监测模块，若nginx编译时包含了监测模块，会同时调用健康监测模块的接口，时时更新健康监测模块的路由表。

性能测试
======

nginx-upsync-module模块，潜在的带来额外的性能开销，比如间隔性的向consul发送请求，由于间隔比较久，且每个请求相当于nginx的一个客户端请求，所以影响有限。基于此，在相同的硬件环境下，使用此模块和不使用此模块简单做了性能对比。

#####基本环境：

```
硬件环境：Intel(R) Xeon(R) CPU E5645 @ 2.40GHz 12 核 
系统环境：centos6.5；
work进程数：8个；   
压测工具：wrk；
压测命令：./wrk -t8 -c100 -d5m --timeout 3s http://$ip:8888/proxy_test
```

#####压测数据：

| - | 总请求 | qps |
| --- | --- | --- |
| nginx(official) | 14802527 | 49521 |
| nginx(upsync) | 14789843 | 48916 |

其中nginx(official)是官方nginx，不执行reload下的测试数据；nginx(upsync)是基于upsync模块，每隔10s钟向consul注册／注销一台机器的数据；从数据可以看出，通过upsync模块做扩缩容操作，对性能的影响有限，可以忽略不计。

应用案例
======

本模块首先应用于平台的remind业务，qps量约为7000+左右。下面是对本业务灰度的基本数据：

#####请求量变化：

| - | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| reload | 7723 | 7583 | 7833 | 7680 | 7809 | 7682 | 6924 | 7081 | 7207 | 7232 | 7486 | 7571 | 7465 |
| upsync | 7782 | 7705| 7772 | 7810 | 7899 | 7978 | 7858 | 7934 | 7994 | 7731 | 7824 | 7648 | 7888 |

<img src="https://github.com/weibocom/nginx-upsync-module/raw/master/doc/images/upsync-vs-reload-qps.png" alt="upsync-vs-reload-qps" height="50%" width="60%">

#####平均耗时变化：

| - | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| reload | 12.102 | 15.108 | 11.443 | 9.426 | 10.178 | 10.605 | 15.253 | 14.315 | 14.762 | 8.392 | 14.385 | 32.335 | 15.277 |
| upsync | 9.586 | 11.963 | 8.694 | 9.676 | 10.616 | 10.335 | 9.766 | 9.406 | 8.943 | 10.971 | 8.080 | 9.185 | 12.055 |

<img src="https://github.com/weibocom/nginx-upsync-module/raw/master/doc/images/upsync-vs-reload-cost.png" alt="upsync-vs-reload-cost" height="50%" width="60%">

从数据可以得出，reload操作时造成nginx的请求处理能力下降约10%，nginx本身的耗时会增长50%+。若是频繁的扩容缩容，reload操作造成的开销会更加明显。

平台为了应对元旦期间的流量峰值，基于本平台的dcp系统，于元旦晚上批量部署阿里云实例，应用此模块进行了百余次的扩容、缩容操作，服务稳定，没有出现服务的波动。另本模块可以应用于对资源的弹性调度系统内，同时可以应用于临时流量突增的场景。

#####参考附录：
[1] http://tengine.taobao.org/document_cn/http_upstream_dynamic_cn.html;

[2] https://www.hashicorp.com/blog/introducing-consul-template.html;

[3] https://www.nginx.com/blog/dynamic-reconfiguration-with-nginx-plus;

[4] https://github.com/alibaba/tengine/issues/595;

[5] https://github.com/xiaokai-wang/nginx-upconf-module;
