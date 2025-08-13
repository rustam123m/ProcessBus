#!/bin/bash

NIC_PORTS="-a 07:00.0 -a 06:00.0"
PAGES_PREFIX="redirect1"
CORE=5

if [[ $1 == "--second" ]]; then
	NIC_PORTS="-a 05:00.0 -a 04:00.0"
	PAGES_PREFIX="redirect2"
	CORE=6
fi

./bin/pkt_redirect -l $CORE $NIC_PORTS --huge-dir=/mnt/${PAGES_PREFIX} --file-prefix=${PAGES_PREFIX}

