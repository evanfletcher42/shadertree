#!/bin/bash

export DISPLAY=:0
xhost +SI:localuser:deck
cat testshader.glsl | ./runshader /dev/ttyACM1
