#!/bin/sh

[[ -d "_nginx" ]] || {
    echo "run: ./fetch NGINX_VERSION" >&2
    exit 1
}

cd _nginx || exit 1

exec make
