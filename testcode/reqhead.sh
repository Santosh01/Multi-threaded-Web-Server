#!/bin/sh
# modify following to get the responce of HEAD request
PORT_NUM=8080
FILE_NAME=README
SERVER=localhost

echo ""
echo "getting HEAD information of file http://$SERVER:$PORT_NUM/$FILE_NAME"
echo ""

curl -i -X HEAD http://$SERVER:$PORT_NUM/$FILE_NAME
