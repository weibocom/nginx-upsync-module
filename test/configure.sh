#!/bin/sh

if [[ ! -d "_nginx" ]]; then
    echo "run: ./fetch NGINX_VERSION" >&2
    exit 1
fi

cd _nginx || exit 1

exec ./configure --add-module=../..
