#!/bin/sh

TEST_NGINX_USE_HUP=1 TEST_NGINX_BINARY=/usr/local/nginx/sbin/nginx prove -r t
