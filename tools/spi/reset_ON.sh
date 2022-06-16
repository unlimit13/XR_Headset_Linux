#!/bin/sh


echo 481 > /sys/class/gpio/export
echo 499 > /sys/class/gpio/export


echo out > /sys/class/gpio/gpio481/direction
echo 0 > /sys/class/gpio/gpio481/value
echo out > /sys/class/gpio/gpio499/direction
echo 0 > /sys/class/gpio/gpio499/value

echo 481 > /sys/class/gpio/unexport
echo 499 > /sys/class/gpio/unexport