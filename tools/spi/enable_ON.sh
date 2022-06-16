#!/bin/sh

echo 457 > /sys/class/gpio/export
echo 462 > /sys/class/gpio/export

echo out > /sys/class/gpio/gpio457/direction
echo 1 > /sys/class/gpio/gpio457/value
echo out > /sys/class/gpio/gpio462/direction
echo 1 > /sys/class/gpio/gpio462/value

echo 457 > /sys/class/gpio/unexport
echo 462 > /sys/class/gpio/unexport