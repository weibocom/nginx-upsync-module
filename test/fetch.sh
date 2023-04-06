#!/bin/sh

if [ -d "_nginx" ]; then
    exit
fi

wget -q http://nginx.org/download/nginx-${1}.tar.gz || exit 1
tar -xzf nginx-${1}.tar.gz || exit 1
exec mv  nginx-${1} _nginx
