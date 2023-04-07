#!/usr/bin/env bash

if [ "$1" != "" ]; then
    wget -q https://releases.hashicorp.com/consul/${1}/consul_${1}_linux_amd64.zip || exit 1
    unzip consul_${1}_linux_amd64.zip || exit 1
fi

./consul agent -server -bootstrap -data-dir=/tmp/consul -bind=127.0.0.1
