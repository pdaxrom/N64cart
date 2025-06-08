#!/bin/sh

openssl req -x509 -nodes -days 365 -newkey rsa:2048 -keyout server.pem -out server.pem
