#!/bin/sh
if [ -e /sys/class/net/eth0 ]; then
      M=$(cat /sys/class/net/eth0/address)
else
      M=$(cat /sys/class/net/wlan0/address)
fi
INFO=$(cat /proc/cpuinfo | grep Serial)
R=$(echo $INFO | tr " " "\n" | tail -1)
echo 324301:ab:bf:$M:sm:tc:$R'72930'
