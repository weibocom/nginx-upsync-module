#!/bin/sh

dir=$( dirname $0 )
if [[ "${dir}" == "." ]]; then
    dir=$( pwd )
fi

TEST_NGINX_USE_HUP=1 TEST_NGINX_BINARY=${dir}/_nginx/objs/nginx prove -r t
