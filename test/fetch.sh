#!/bin/sh

[[ -e "_nginx" ]] && exit

wget http://nginx.org/download/nginx-${1}.tar.gz || exit 1
tar -xzf nginx-${1}.tar.gz || exit 1
exec mv  nginx-${1} _nginx
