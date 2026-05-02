#!/bin/bash

NIC_PORTS=""

if [[ $1 == "--sfp" ]]; then
	NIC_PORTS="-a 0B:00.0 -a 0C:00.0"
	echo "This is SFP mode! $NIC_PORTS"
else
	NIC_PORTS="-a 07:00.0 -a 08:00.0"
	echo "This is RJ45 mode! $NIC_PORTS"
fi

./bin/delay_meter -l 4 $NIC_PORTS --huge-dir=/mnt/delay_meter --file-prefix=delay_meter

