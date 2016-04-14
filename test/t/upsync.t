use warnings;
use strict;

use Test::More;

BEGIN { use FindBin; chdir($FindBin::Bin); }


use lib 'lib';
use File::Path;
use Test::Nginx;


my $NGINX = defined $ENV{TEST_NGINX_BINARY} ? $ENV{TEST_NGINX_BINARY}
: '../../nginx/objs/nginx';
my $t = Test::Nginx->new()->plan(40);

sub mhttp_get($;$;$;%) {
    my ($url, $host, $port, %extra) = @_;
    return mhttp(<<EOF, $port, %extra);
GET $url HTTP/1.0
Host: $host

EOF
}

sub mhttp_put($;$;$;%) {
    my ($url, $body, $port, %extra) = @_;
    my $len = length($body);
    return mhttp(<<EOF, $port, %extra);
PUT $url HTTP/1.0
Host: localhost
Content-Length: $len

$body
EOF
}

sub mhttp_delete($;$;%) {
    my ($url, $port, %extra) = @_;
    return mhttp(<<EOF, $port, %extra);
DELETE $url HTTP/1.0
Host: localhost

EOF
}

sub get_dump_content($;%) {
    my ($file_name, %extra) = @_;

    my $content;
    if ( open my $fd, '<', $file_name ) {
        $content = do { local $/; <$fd> };

        close $fd;

    } else {
        $content = 'The file could not be opened.';
    }

    return $content;
}

sub mrun($;$) {
    my ($self, $conf) = @_;

    my $testdir = $self->{_testdir};

    if (defined $conf) {
        my $c = `cat $conf`;
        $self->write_file_expand('nginx.conf', $c);
    }

    my $pid = fork();
    die "Unable to fork(): $!\n" unless defined $pid;

    if ($pid == 0) {
        my @globals = $self->{_test_globals} ?
        () : ('-g', "pid $testdir/nginx.pid; "
            . "error_log $testdir/error.log debug;");
        exec($NGINX, '-c', "$testdir/nginx.conf", '-p', "$testdir",
            @globals) or die "Unable to exec(): $!\n";
    }

    # wait for nginx to start

    $self->waitforfile("$testdir/nginx.pid")
        or die "Can't start nginx";

    $self->{_started} = 1;
    return $self;
}

###############################################################################

select STDERR; $| = 1;
select STDOUT; $| = 1;

warn "your test dir is ".$t->testdir();

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;

worker_processes 1;

events {
    accept_mutex off;
}

http {

    upstream test {
        upsync 127.0.0.1:8500/v1/kv/upstreams/test upsync_interval=50ms upsync_timeout=6m upsync_type=consul;
        upsync_dump_path /tmp/servers_test.conf;

        server 127.0.0.1:8088 weight=10 max_fails=3 fail_timeout=10;
    }

    upstream backend {
        server 127.0.0.1:8090 weight=10 max_fails=3 fail_timeout=10;
    }

    server {
        listen   8080;

        location / {
            proxy_pass http://$host;
        }

        location /upstream_list {
            upstream_show;
        }
    }
}
EOF

mrun($t);

###############################################################################
my $rep;
my $dump;

like(mhttp_delete('/v1/kv/upstreams/test/127.0.0.1:8089', 8500), qr/true/m, '2015-12-27 18:21:37');
like(mhttp_delete('/v1/kv/upstreams/test/127.0.0.1:8088', 8500), qr/true/m, '2015-12-27 18:20:37');

sleep(1);

like(mhttp_delete('/v1/kv/upstreams/test?recurse', 8500), qr/true/m, '2015-12-27 18:22:37');

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8089', '', 8500), qr/true/m, '2015-12-27 17:50:35');

$rep = qr/
Upstream name: test;Backend server counts: 1
        server 127.0.0.1:8089 weight=1 max_fails=2 fail_timeout=10;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 17:43:30');

###############################################################################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8088', '{"weight":10,"max_fails":3,"fail_timeout":10}', 8500), qr/true/m, '2015-12-27 17:42:35');

$rep = qr/
Upstream name: test;Backend server counts: 2
        server 127.0.0.1:8089 weight=1 max_fails=2 fail_timeout=10;
        server 127.0.0.1:8088 weight=10 max_fails=3 fail_timeout=10;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 18:17:53');

like(mhttp_get('/upstream_list', 'localhost', 8080), $rep, '2015-04-14 18:17:53');

#########################

like(mhttp_delete('/v1/kv/upstreams/test/127.0.0.1:8089', 8500), qr/true/m, '2015-12-27 18:24:37');

$rep = qr/
Upstream name: test;Backend server counts: 1
        server 127.0.0.1:8088 weight=10 max_fails=3 fail_timeout=10;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 18:30:40');

#########################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8089', '{"weight":20,"max_fails":0,"fail_timeout":30}', 8500), qr/true/m, '2015-12-27 18:31:39');

$rep = qr/
Upstream name: test;Backend server counts: 2
        server 127.0.0.1:8088 weight=10 max_fails=3 fail_timeout=10;
        server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 18:32:53');

########################

like(mhttp_delete('/v1/kv/upstreams/test/127.0.0.1:8088', 8500), qr/true/m, '2015-12-27 18:33:37');

$rep = qr/
Upstream name: test;Backend server counts: 1
        server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 18:34:40');

like(mhttp_get('/upstream_list', 'localhost', 8080), $rep, '2015-04-14 18:34:40');

#######################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8088', '{"weight":20,"max_fails":0,"fail_timeout":30}', 8500), qr/true/m, '2015-12-27 18:35:35');

$rep = qr/
Upstream name: test;Backend server counts: 2
        server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30;
        server 127.0.0.1:8088 weight=20 max_fails=0 fail_timeout=30;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 18:41:40');

#######################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8088', '{"weight":40}', 8500), qr/true/m, '2015-12-27 18:42:35');

$rep = qr/
Upstream name: test;Backend server counts: 2
        server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30;
        server 127.0.0.1:8088 weight=40 max_fails=0 fail_timeout=30;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 18:43:40');

like(mhttp_get('/upstream_list', 'localhost', 8080), $rep, '2015-04-14 18:43:40');

#######################

like(mhttp_delete('/v1/kv/upstreams/test?recurse', 8500), qr/true/m, '2015-12-27 18:22:33');

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 18:44:51');

like(mhttp_get('/upstream_list', 'localhost', 8080), $rep, '2016-04-14 18:44:51');

#######################

$dump = qr/server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30s;
server 127.0.0.1:8088 weight=40 max_fails=0 fail_timeout=30s;
/m;

like(get_dump_content('/tmp/servers_test.conf'), $dump, '2015-12-27 18:51:35');

#######################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8088', '{"weight":40,"max_fails":3,"fail_timeout":20, "down":1}', 8500), qr/true/m, '2016-03-15 18:35:35');

sleep(1);

$dump = qr/server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30s;
server 127.0.0.1:8088 weight=40 max_fails=3 fail_timeout=20s down;
/m;

like(get_dump_content('/tmp/servers_test.conf'), $dump, '2016-03-15 18:51:35');

#######################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8088', '{"weight":40,"max_fails":0,"fail_timeout":30, "down":0}', 8500), qr/true/m, '2016-03-15 17:35:35');

sleep(1);

$dump = qr/server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30s;
server 127.0.0.1:8088 weight=40 max_fails=0 fail_timeout=30s;
/m;

like(get_dump_content('/tmp/servers_test.conf'), $dump, '2016-03-15 17:51:35');

$t->stop();

##############################################################################

$t->write_file_expand('nginx.conf', <<'EOF');

%%TEST_GLOBALS%%

daemon off;

worker_processes auto;

events {
    accept_mutex off;
}

http {

    upstream test {
        upsync 127.0.0.1:8500/v1/kv/upstreams/test upsync_interval=50ms upsync_timeout=6m upsync_type=consul;
        upsync_dump_path /tmp/servers_test.conf;

        server 127.0.0.1:8088 weight=10 max_fails=3 fail_timeout=10;
    }

    upstream backend {
        server 127.0.0.1:8090 weight=10 max_fails=3 fail_timeout=10;
    }

    server {
        listen   8080;

        location / {
            proxy_pass http://$host;
        }

        location /upstream_list {
            upstream_show;
        }
    }
}
EOF

mrun($t);

###############################################################################

like(mhttp_delete('/v1/kv/upstreams/test/127.0.0.1:8088', 8500), qr/true/m, '2015-12-27 19:20:37');
like(mhttp_delete('/v1/kv/upstreams/test/127.0.0.1:8089', 8500), qr/true/m, '2015-12-27 19:21:37');

sleep(1);

like(mhttp_delete('/v1/kv/upstreams/test?recurse', 8500), qr/true/m, '2015-12-27 19:49:37');

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8089', '', 8500), qr/true/m, '2015-12-27 19:25:35');

$rep = qr/
Upstream name: test;Backend server counts: 1
        server 127.0.0.1:8089 weight=1 max_fails=2 fail_timeout=10;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 19:20:30');

like(mhttp_get('/upstream_list', 'localhost', 8080), $rep, '2015-04-14 19:20:30');

###############################################################################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8088', '{"weight":10,"max_fails":3,"fail_timeout":10}', 8500), qr/true/m, '2015-12-27 17:50:35');

$rep = qr/
Upstream name: test;Backend server counts: 2
        server 127.0.0.1:8089 weight=1 max_fails=2 fail_timeout=10;
        server 127.0.0.1:8088 weight=10 max_fails=3 fail_timeout=10;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 19:26:53');

like(mhttp_get('/upstream_list', 'localhost', 8080), $rep, '2015-04-14 19:26:53');

###########################

like(mhttp_delete('/v1/kv/upstreams/test/127.0.0.1:8089', 8500), qr/true/m, '2015-12-27 19:28:37');

$rep = qr/
Upstream name: test;Backend server counts: 1
        server 127.0.0.1:8088 weight=10 max_fails=3 fail_timeout=10;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 19:30:40');

like(mhttp_get('/upstream_list', 'localhost', 8080), $rep, '2015-04-14 19:30:40');

###########################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8089', '{"weight":20,"max_fails":0,"fail_timeout":30}', 8500), qr/true/m, '2015-12-27 19:31:39');

$rep = qr/
Upstream name: test;Backend server counts: 2
        server 127.0.0.1:8088 weight=10 max_fails=3 fail_timeout=10;
        server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30;
/m;

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 19:32:53');

like(mhttp_get('/upstream_list', 'localhost', 8080), $rep, '2015-04-14 19:32:53');

##########################

like(mhttp_delete('/v1/kv/upstreams/test/127.0.0.1:8088', 8500), qr/true/m, '2015-12-27 19:35:37');

$rep = qr/
Upstream name: test;Backend server counts: 1
        server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 19:37:40');

###########################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8088', '{"weight":20,"max_fails":0,"fail_timeout":30}', 8500), qr/true/m, '2015-12-27 19:39:35');

$rep = qr/
Upstream name: test;Backend server counts: 2
        server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30;
        server 127.0.0.1:8088 weight=20 max_fails=0 fail_timeout=30;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 19:47:40');

###########################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8088', '{"weight":40}', 8500), qr/true/m, '2015-12-27 19:48:35');

$rep = qr/
Upstream name: test;Backend server counts: 2
        server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30;
        server 127.0.0.1:8088 weight=40 max_fails=0 fail_timeout=30;
/m;

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 19:49:40');

like(mhttp_get('/upstream_list', 'localhost', 8080), $rep, '2015-04-14 19:49:40');

###########################

like(mhttp_delete('/v1/kv/upstreams/test?recurse', 8500), qr/true/m, '2015-12-27 19:50:37');

sleep(1);

like(mhttp_get('/upstream_list?test', 'localhost', 8080), $rep, '2015-12-27 19:51:40');

like(mhttp_get('/upstream_list', 'localhost', 8080), $rep, '2015-04-14 19:51:40');

##########################

$dump = qr/server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30s;
server 127.0.0.1:8088 weight=40 max_fails=0 fail_timeout=30s;
/m;

like(get_dump_content('/tmp/servers_test.conf'), $dump, '2015-12-27 19:53:35');

##########################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8088', '{"weight":40,"max_fails":3,"fail_timeout":20, "down":1}', 8500), qr/true/m, '2016-03-15 19:39:35');

sleep(1);

$dump = qr/server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30s;
server 127.0.0.1:8088 weight=40 max_fails=3 fail_timeout=20s down;
/m;

like(get_dump_content('/tmp/servers_test.conf'), $dump, '2016-03-15 19:53:35');

##########################

like(mhttp_put('/v1/kv/upstreams/test/127.0.0.1:8088', '{"weight":40,"max_fails":3,"fail_timeout":20, "down":0}', 8500), qr/true/m, '2016-03-15 20:39:35');

sleep(1);

$dump = qr/server 127.0.0.1:8089 weight=20 max_fails=0 fail_timeout=30s;
server 127.0.0.1:8088 weight=40 max_fails=3 fail_timeout=20s;
/m;

like(get_dump_content('/tmp/servers_test.conf'), $dump, '2016-03-15 20:53:35');

$t->stop();

###############################################################################

sub mhttp($;$;%) {
    my ($request, $port, %extra) = @_;
    my $reply;
    eval {
        local $SIG{ALRM} = sub { die "timeout\n" };
        local $SIG{PIPE} = sub { die "sigpipe\n" };
        alarm(2);
        my $s = IO::Socket::INET->new(
            Proto => "tcp",
            PeerAddr => "127.0.0.1:$port"
        );
        log_out($request);
        $s->print($request);
        local $/;
        select undef, undef, undef, $extra{sleep} if $extra{sleep};
        return '' if $extra{aborted};
        $reply = $s->getline();
        alarm(0);
    };
    alarm(0);
    if ($@) {
        log_in("died: $@");
        return undef;
    }
    log_in($reply);
    return $reply;
}
