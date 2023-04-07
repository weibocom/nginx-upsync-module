#!/usr/bin/env bash

dir=$( pwd )

cd ${dir}/test || exit 1

TEST_NGINX_USE_HUP=1 TEST_NGINX_BINARY=${dir}/_nginx/objs/nginx prove -r t
