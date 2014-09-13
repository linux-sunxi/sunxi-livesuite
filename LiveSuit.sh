#!/bin/bash

APP=LiveSuit
TOP_DIR=`pwd`

MACHINE=$(uname -m)
if [ ${MACHINE} == 'x86_64' ]; then
	BIN_DIR="x86-64"	
elif [ ${MACHINE} == 'i686' ]; then
	BIN_DIR="x86"
else
	echo "Error: unknown architecture ${MACHINE}"
	exit
fi

echo "Starting ${BIN_DIR}/${APP}."
echo ""

LD_LIBRARY_PATH=${TOP_DIR}/${BIN_DIR}/ ${BIN_DIR}/${APP}
