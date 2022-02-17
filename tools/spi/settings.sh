#!/bin/sh

#echo 457 > /sys/class/gpio/export
#echo 462 > /sys/class/gpio/export

echo 481 > /sys/class/gpio/export
echo 499 > /sys/class/gpio/export

#echo out > /sys/class/gpio/gpio457/direction
#echo 1 > /sys/class/gpio/gpio457/value
#echo out > /sys/class/gpio/gpio462/direction
#echo 1 > /sys/class/gpio/gpio462/value

#echo 457 > /sys/class/gpio/unexport
#echo 462 > /sys/class/gpio/unexport

echo out > /sys/class/gpio/gpio481/direction
echo 1 > /sys/class/gpio/gpio481/value
echo out > /sys/class/gpio/gpio499/direction
echo 1 > /sys/class/gpio/gpio499/value

echo 481 > /sys/class/gpio/unexport
echo 499 > /sys/class/gpio/unexport

./dp_configure_L -v
./dp_configure_R -v