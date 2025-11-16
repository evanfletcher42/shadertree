#!/bin/bash

# give this shell access to the display, for running shaders
export DISPLAY=:0
xhost +SI:localuser:deck

python runner_process.py
