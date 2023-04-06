#!/bin/sh

dir=$( dirname $0 )
[ "${dir}" == "." ] && dir=$( pwd )

TEST_NGINX_USE_HUP=1 TEST_NGINX_BINARY=${dir}/_nginx/objs/nginx prove -r t
