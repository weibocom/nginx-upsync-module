# vi:filetype=perl

use lib 'lib';
use Test::Nginx::LWP;

sub consul_add {
    my ($host, $iport) = @_;
    my $url = "http://127.0.0.1:8500/v1/kv/upstreams/$host/127.0.0.1:$iport";

    my $ua = new LWP::UserAgent;

    my $request = new HTTP::Request('PUT', $url);
    my $response = $ua->request($request);
    if ($response->is_success) {
         print $response->content;
     } else {
        print $response->error_as_HTML;
    }
}


sub consul_del {
    my ($host, $iport) = @_;
    my $url = "http://127.0.0.1:8500/v1/kv/upstreams/$host/127.0.0.1:$iport";

    my $ua = new LWP::UserAgent;

    my $request = new HTTP::Request('DELETE', $url);
    my $response = $ua->request($request);
    if ($response->is_success) {
         print $response->content;
     } else {
        print $response->error_as_HTML;
    }
}

plan tests => repeat_each(1) * 2 * blocks();

my @server_ports = ("10001", "10002");

foreach my $port (@server_ports) {
    consul_add("backend", $port);
    consul_add("test", $port);
}

consul_del("test", "10002");

no_root_location();

run_tests();


__DATA__

=== TEST 1: add upstream backend server
--- http_config
upstream backend {
    server 127.0.0.1:11111;

    consul 127.0.0.1:8500/v1/kv/upstreams/backend update_timeout=6m update_interval=2s strong_dependency=off;
}

upstream test {
    server 127.0.0.1:11111;

    consul 127.0.0.1:8500/v1/kv/upstreams/test update_timeout=6m update_interval=2s strong_dependency=off;
}

server {
    listen 8090;

    location / {
        root   html;
        index  index.html index.htm;
    }
}

--- config
    location /backend {
        proxy_pass http://backend;
    }

    location /test {
        proxy_pass http://test;
    }

    location /upstream_show {
        upstream_show;
    }

--- request
    GET /upstream_show?backend
--- response_body
Upstream name: backend ;Backend server counts: 2
        server: 127.0.0.1:10001
        server: 127.0.0.1:10002

=== TEST 2: del upstream test server
--- http_config
upstream backend {
    server 127.0.0.1:11111;

    consul 127.0.0.1:8500/v1/kv/upstreams/backend update_timeout=6m update_interval=2s strong_dependency=off;
}

upstream test {
    server 127.0.0.1:11111;

    consul 127.0.0.1:8500/v1/kv/upstreams/test update_timeout=6m update_interval=2s strong_dependency=off;
}

server {
    listen 8090;

    location / {
        root   html;
        index  index.html index.htm;
    }
}

--- config
    location /backend {
        proxy_pass http://backend;
    }

    location /test {
        proxy_pass http://test;
    }

    location /upstream_show {
        upstream_show;
    }

--- request
    GET /upstream_show?test
--- response_body
Upstream name: test ;Backend server counts: 1
        server: 127.0.0.1:10001
