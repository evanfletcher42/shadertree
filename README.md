# shadertree

<img width="1280" height="640" alt="shadertree" src="https://github.com/user-attachments/assets/828fefa0-d9ee-4836-b6c3-7497550b7adf" />

Shadertree is a collaborative-art holiday tree made out of thousands of 3D-scanned addressible LEDs. From a web browser, you can write and preview GLSL vertex shaders that write color attributes, which render out to the LEDs live! 

This code is meant to run on a Steam Deck next to the tree, which is nice for kiosk UI, but anything that can run a website + has USB ports + can make a surfaceless EGL context should also work. 

LEDs are driven over USB using cnlohr's [x32-ws2812-usbhs-breakout](https://github.com/cnlohr/x32-ws2812-usbhs-breakout) hardware and firmware. 


## How it works

- **LED reconstruction** (`reconstruct/`): A C++ bundle-adjustment solver that decodes LED blinks from the tree & solves for the 3D position of every LED
- **Web site** (`stweb/`): A Django app with the shader editor & tools for playlist management
- **Shader Runner** (`runner_process.py`, `runshader.cpp`) - a daemon that runs submitted shaders on the GPU (EGL), pulls out vertex color attributes via transform feddback, and streams colors over USB to two WS2812 driver boards.


## Writing shaders

Shaders are written on the site with a live Shadertoy-like editor and preview. You write a vertex shader `main()` that writes to a vertex color attribute. For example: 

```glsl
#version 300 es
layout(location = 0) in vec3 iPos;  // LED 3D position: +y up, bottom center ~(0,-1,0), tip ~(0,1,0)
layout(location = 1) in uint iIdx;  // LED address in the string (raw hardware addresses; not contiguous)
uniform float iTime;                // seconds since the shader started
uniform int iFrame;                 // frame counter, from 0
out vec3 outColor;                  // output: RGB color, 0 to 1

void main()
{
    // Animate bands of color climbing up the tree
    outColor = vec3( 0.5 + 0.5 * sin( iPos.y * 6.0 - iTime * 2.0 ), 0.1, 0.3 );
}
```

## Setup / Install

Shadertree's website and shader runner are built to run on a Steam Deck running SteamOS 3.9 or newer. However, almost none of it is Steam Deck specific. Many Linux systems would likely work just as well.

LED reconstruction *can* be run on a Steam Deck, but it's likely better to run this on a faster PC. Reconstruction code in this repository works on Windows and should run on most Linux distributions. 

## Website Install (Steam Deck)

From desktop mode, open a terminal (Konsole) and run:
```bash
sudo passwd deck  # only if the deck user has no password yet

git clone https://github.com/evanfletcher42/shadertree.git ~/shadertree
cd ~/shadertree

./install.sh
```

This sets a root password (you'll need one), clones this repository into your home directory, and runs an install script.

On the first run, this script:
 - Disables read-only mode in the SteamOS filesystem (`sudo steamos-readonly disable`)
 - Installs a compiler and headers needed to build the shader runner
 - Sets up deployment (`/var/www/shadertree`) and backup (`~/shadertree-backups`) directories
 - Sets up udev rules to talk to the LED board via USB
 - Installs [gunicorn](https://gunicorn.org/)
 - Installs [nginx](https://nginx.org/) in a rootless podman container
 - Adds a firewall rule for http (port 80)
 - Configures loginctl to allow containers to run at boot without user login

This can take a few minutes the first time. 

If you are installing on a new machine, but would like to start from an existing database or backup, run:
```bash
./install.sh --import-db path/to/db.sqlite3
```
Note this only does anything if there is not already a deployed database. For restoring a DB from a backup, see [Backups](#backups).

### Configuration

1. Edit `/var/www/shadertree/local.env` and set `SHADERTREE_ALLOWED_HOSTS` to the names and IPs people use to reach the site, comma-separated (for example `shadertree.example.com,10.0.0.5`).
2. Then run: `sudo systemctl restart gunicorn`
3. Create a django admin login: `cd /var/www/shadertree && venv/bin/python manage.py createsuperuser` - this is your login for the site at `shadertree.example.com/admin/`

### After a SteamOS Update

SteamOS updates tend to wipe build tools and configurations under `/etc` (services, udev rules, firewall rules, ...).

If things seem broken after an OS update, just run `./install.sh` again. This will put back missing things, and will not touch your deployed db or backups. 

### Scanning the Tree

See the section [LED Reconstruction](#led-reconstruction) below. 


## Development Setup

### Ubuntu, Arch (Steam Deck)
Run:
```bash
./setup-dev.sh
```

This will install C++ solver dependencies with apt or pacman & create a `.venv`.

### Windows

C++ development requires Visual Studio 2022 or newer & Python 3.12.

Run:
```powershell
powershell -ExecutionPolicy Bypass -File setup-dev.ps1
```
and follow any instructions to install missing components. The script will then install dependencies and set up a Python `.venv`. 

If you have not already set one, this will also set `VCPKG_ROOT` to Visual Studio's bundled vcpkg. 


## Running the site locally

From `stweb/`, activate the `.venv`, then:
```
python manage.py migrate
python manage.py createsuperuser
python manage.py runserver --insecure
```
The site will be at http://127.0.0.1:8000/. 

### Building runshader

Run `make` in the repository root. 

## Deploying changes

On the Steam Deck, in the cloned repository, run:

```bash
git pull
./deploy.sh 
```

This will:
1. Build the `runshader` binary
2. Sync `stweb/` to `/var/www/shadertree` (deleting files no longer in the repo)
3. Back up the db
4. Install any new dependencies in the deployed `.venv`
5. Migrates the db & collects any static files
6. Restarts the site and the shader runner service. 

NOTE: If your changes modify `install.sh` or anything under `deploy/`, run `./install.sh` instead. This will also deploy. 

## Logs and status

```
systemctl status gunicorn shadertree-runner shadertree-backup.timer
systemctl --user status shadertree-nginx
journalctl -u shadertree-runner -f
```

## Backups

Backups go in `~/shadertree-backups/` hourly and on every deploy. The most recent 168 copies (~7 days worth) are kept. 

To restore from a backup:

1. `sudo systemctl stop gunicorn shadertree-runner`
2. copy the backup over `/var/www/shadertree/db.sqlite3`
3. delete `db.sqlite3-wal` and `db.sqlite3-shm` 
4. `sudo systemctl start gunicorn shadertree-runner` 

## LED reconstruction

### Building the solver

`ledreconstruct` (under `/reconstruct/`) builds with CMake (3.22+) and a C++17 compiler.

Run the setup script for your platform first (see [Setup](#setup)).

Dependencies (should be taken care of automatically):

 - Ceres 2.2+
 - Eigen 3.4+
 - OpenCV 4.6+ 
 - FFmpeg ( for video decode with OpenCV 5+)
 - jsoncpp 
 - cxxopts 3.1+

### Windows

Powershell:
```powershell
cd reconstruct
cmake --preset windows
cmake --build --preset windows-release
```

`vcpkg` will pull and build dependencies from `vcpkg.json` when cmake is configured (`--preset windows`). This will be slow - ceres & opencv are big - but should be cached after the first run. 

Executables will end up in `build\windows\Release`. 

If you prefer, you can also open `reconstruct\` as a folder in Visual Studio, or the repo as a whole in Visual Studio Code. 

For a faster debuggable build, the "release with debug symbols" target is recommended:
```powershell
cmake --build --preset windows-relwithdebinfo
```
The ordinary Debug target will work, but the solver will be painfully slow.

**Ubuntu 24.04 and Arch:**
```bash
cd reconstruct
cmake --preset linux
cmake --build --preset linux -j
```
Executable will end up under `build/linux/`.

### SteamOS

Build inside an Arch container. Run:
```bash
distrobox create --name arch --image archlinux:latest
distrobox enter arch
```

and follow the Arch steps above.

### Other Platforms

Use vcpkg for dependencies:
```
cmake --preset linux -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```
Beyond that, you're on your own. 

### Calibrating the camera

The solver needs a calibration for the capture camera (`--calibration`). Tools are provided under `/reconstruct/tools` to make one:

1. `python reconstruct/tools/make_charuco_target.py charuco.png`. Show this image full-screen on a monitor, or print it out.
2. Film the target (video) with the capture camera, at the zoom and resolution you'll use for capturing the tree. Cover many angles and distances. Make sure the target completely fills the image frame. 
3. `python reconstruct/tools/calibrate_charuco_from_video.py <videos> --output calib.json`

Keep `calib.json` with your captures.

### Capturing and Solving

On the Steam Deck:
```bash
sudo systemctl stop shadertree-runner
```

Then, from the repository root (`~/shadertree`), run:
```bash
(cd /var/www/shadertree && ./runshader) < shaderrunner/ledcodes.glsl
```

This shader blinks out a series of (8,4,4)_2 Hadamard codes & sync signals for a 60 FPS camera capture. You may need to edit `ledcodes.glsl` for cameras with different timing. 

Film this using a stationary camera (e.g. on a tripod), from many different perspectives. The specific captures needed will depend on your camera and your tree. The goal is to capture at least 2 or 3 clear views of every LED from different perspectives. For reference, the 2025 tree used ~150 perspectives, but this was probably excessive. 

Set your camera exposure **low**. The image should be mostly dark, except for the LEDs. 

Once captured, stop `runshader` with CTRL+C, copy all your videos to your PC, then run:
```bash
./ledreconstruct --videos <video_dir> --dump-observations <video_dir>/obs --calibration calib.json --output points.json --site <site_name>
```

Once solved, log into the django admin on the site (at `/admin/`), then paste the contents of `<site_name>.ledcoords.json` into the `LedCoordsConfig` entry. 

When finished, run:
```
sudo systemctl start shadertree-runner
```

Your reconstructed tree should appear on the site in preview windows & be used for shaders. 

## Credits

Shadertree is a project by Evan Fletcher and Charles Lohr. 

Evan bought the first string of LEDs for this project back in ~2017. At first, these were used as "normal" animated RGB holiday lights. 

The first version of the 3D reconstruction solver was written in 2022, with a 500-LED tree driven by fixed firmware on an Arduino. Reconstructed points were in a header file & effects were animated with FastLED. Code for that version of the tree is still available here: https://github.com/evanfletcher42/3dBlinkenlights 

This fixed-function tree was used at the Valve office on 12F / 13F in 2022 and 2023. 

"Shadertree," as in the website editor and collaborative art project, first came online at the Valve office for holiday 2024, again using the 500-LED tree. One of the first shaders written for it was, appropriately, the [Source Engine "Missing Texture" shader](https://developer.valvesoftware.com/wiki/Missing_content). 

Charles joined the project for holiday 2025, contributing [power electronics & LED drivers](https://github.com/cnlohr/x32-ws2812-usbhs-breakout) for a much larger tree, set up in the cafeteria on 15F. 

This project was open sourced in November 2026, with a bunch of QoL upgrades & some slight hardware tweaks for the tree. Mostly, we waited until the building where the office is started putting up holiday decorations. We figured doing this earlier would've been tacky. 

### Third-party code

These files are included in this repository under their own licenses, not the project license:

- [Ace](https://ace.c9.io/) 1.4.12 (`ace.js`, `mode-glsl.js`, `theme-chrome.js`). Copyright (c) 2010, Ajax.org B.V. BSD 3-Clause.
- [gl-matrix](https://glmatrix.net/) 2.7.0. Copyright (c) 2015-2018, Brandon Jones, Colin MacKenzie IV. MIT.
- [Lobster](https://fonts.google.com/specimen/Lobster) font 1.4. Copyright (c) 2010, Pablo Impallari. SIL Open Font License 1.1.
- [x32-ws2812-usbhs-breakout](https://github.com/cnlohr/x32-ws2812-usbhs-breakout). Copyright (c) 2025, Charles Lohr. MIT.

### Dependencies

Shadertree is built with these projects, not included in this repository: 

- Website: [Django](https://www.djangoproject.com/), [gunicorn](https://gunicorn.org/), [nginx](https://nginx.org/)
- Shader runner: [libusb](https://libusb.info/), [EGL](https://www.khronos.org/egl) / [OpenGL ES](https://www.khronos.org/opengles/)
- LED reconstruction: [Ceres Solver](https://github.com/ceres-solver/ceres-solver), [OpenCV](https://opencv.org/), [Eigen](https://eigen.tuxfamily.org/), [jsoncpp](https://github.com/open-source-parsers/jsoncpp), [cxxopts](https://github.com/jarro2783/cxxopts)
- Camera calibration tools: [OpenCV](https://opencv.org/) (Python)

Please refer to these projects for their license terms. 
