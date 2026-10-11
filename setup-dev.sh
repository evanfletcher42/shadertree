#!/bin/sh
# Linux dev setup. Safe to re-run.
# Installs the solver's dependencies (apt or pacman) and creates .venv.
# Usage: ./setup-dev.sh [--no-venv]
set -e
cd "$(dirname "$0")"
SUDO=$( [ "$(id -u)" -eq 0 ] || echo sudo )

if command -v apt-get >/dev/null; then
	$SUDO apt-get update
	$SUDO apt-get install -y cmake g++ libceres-dev libopencv-dev libeigen3-dev libjsoncpp-dev libcxxopts-dev python3-venv
elif command -v pacman >/dev/null; then
	$SUDO pacman -Syu --needed --noconfirm base-devel cmake ceres-solver opencv qt6-base vtk hdf5 eigen jsoncpp cxxopts python
else
	echo "No apt or pacman: install Ceres, Eigen, OpenCV, jsoncpp and cxxopts yourself, or use vcpkg (see README.md)." >&2
	exit 1
fi

if [ "$1" != "--no-venv" ]; then
	[ -d .venv ] || python3 -m venv .venv
	.venv/bin/python -m pip install --quiet --disable-pip-version-check -r stweb/requirements.txt -r reconstruct/tools/requirements.txt
fi

echo "Done. Build the solver with: cd reconstruct && cmake --preset linux && cmake --build --preset linux -j"
