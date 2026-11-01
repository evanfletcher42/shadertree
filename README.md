# shadertree

**TODO: one-line tagline.**

**TODO: photo or GIF of the tree.**

shadertree is a holiday tree lit by thousands of individually addressable LEDs, programmed from a web browser. Anyone on the network can write a short GLSL shader in the online editor, preview it on a 3D model of the tree, and add it to the playlist. The tree plays each shader for 30 seconds, in shuffled order.

Each LED's color comes from a shader evaluated at that LED's real 3D position on the tree. The positions are measured by filming the tree while every LED blinks its own address, then solving for where each one must be.

**TODO: link to the live site, if public.**

## How it works

- **Web site** (`stweb/`): a Django app with the shader editor and its live WebGL preview, the shader library with full version history, the playlist and a "now playing" page for a kiosk screen.
- **Shader runner**: `runner_process.py` picks the next shader from the playlist. `runshader` (`shaderrunner/`, C++) evaluates it for every LED on the GPU (EGL, transform feedback) and streams the colors over USB to two WS2812 driver boards, 32 strands × 215 LEDs each.
- **Hardware**: a Steam Deck runs all of the above. **TODO: describe the LED strings, driver boards and power.**
- **LED reconstruction** (`reconstruct/`): a C++ tool that decodes the blink codes from videos of the tree and solves for the 3D position of every LED with bundle adjustment (Ceres).

## Writing shaders

Every shader starts with this prefix, which the editor shows above your code:

```glsl
#version 300 es
layout(location = 0) in vec3 iPos;  // LED 3D position: +y up, bottom center ~(0,-1,0), tip ~(0,1,0)
layout(location = 1) in uint iIdx;  // LED address in the string (raw hardware addresses; not contiguous)
uniform float iTime;                // seconds since the shader started
uniform int iFrame;                 // frame counter, from 0
out vec3 outColor;                  // output: RGB color, 0 to 1
```

Write a `main()` that sets `outColor`. For example, bands of color rising up the tree:

```glsl
void main()
{
    outColor = vec3( 0.5 + 0.5 * sin( iPos.y * 6.0 - iTime * 2.0 ), 0.1, 0.3 );
}
```

The site's About page has more.

## Repository layout

| Path | Contents |
|---|---|
| `stweb/` | Django site, and `runner_process.py`, which plays the playlist |
| `shaderrunner/` | `runshader` (renders a shader and drives the LEDs), the USB driver, the udev rule, `ledcodes.glsl` (blink codes for reconstruction) |
| `reconstruct/` | `ledreconstruct` (decoder and solver), plus Python tools for camera calibration and synthetic test data |
| `deploy/` | systemd units and nginx config for the Deck |
| `install.sh`, `deploy.sh` | Deck setup and deployment |
| `setup-dev.sh`, `setup-dev.ps1` | development setup for Linux and Windows |

## Running on a Steam Deck

A Steam Deck on SteamOS 3.9 or newer runs the site, the shader runner and an hourly database backup. One script installs everything.

What runs where:
- `/var/www/shadertree/`: the deployed site, its venv, database, settings (`local.env`) and secret key.
- `~/shadertree-backups/`: database backups. `/var` is only 230 MB, so they live in `/home`.
- systemd services: `gunicorn` (the site, on 127.0.0.1:8000), `shadertree-runner` (plays the playlist) and `shadertree-backup.timer`.
- A rootless podman container, `shadertree-nginx`, serves port 80.

To build `runshader`, the script installs a compiler and headers into SteamOS with pacman, turning read-only mode off and back on. It also opens port 80 in the firewall.

### Install

In desktop mode, open a terminal (Konsole) and run:
```
sudo passwd deck            # only if the deck user has no password yet
git clone https://github.com/evanfletcher42/shadertree.git ~/shadertree
cd ~/shadertree
./install.sh
```
The first run installs the build tools and downloads the nginx container, which takes a few minutes. To start from an existing database or backup instead of an empty one, use `./install.sh --import-db path/to/db.sqlite3`. It's only used if there's no database yet.

Then:
1. Edit `/var/www/shadertree/local.env` and set `SHADERTREE_ALLOWED_HOSTS` to the names and IPs people use to reach the site, comma-separated (for example `shadertree.example.com,10.0.0.5`). Other names get a 400 error. Then run `sudo systemctl restart gunicorn`.
2. Create an admin login: `cd /var/www/shadertree && venv/bin/python manage.py createsuperuser`. The admin is at `/admin/`.
3. Without the LED boards attached, set `SHADERTREE_NO_USB=1` in `local.env` and run `sudo systemctl restart shadertree-runner`.

### After a SteamOS update

Run `./install.sh` again. Updates remove the build tools, can reset what the script put in `/etc` (services, udev rule, firewall rule), and can replace the system Python. The script puts all of that back and rebuilds the venv if needed. Your database, settings and backups are kept.

### Deploying changes

`git pull`, then `./deploy.sh`. It builds `runshader`, syncs `stweb/` to `/var/www/shadertree/` (deleting files that are no longer in the repo), backs up the database, installs requirements, migrates, collects static files and restarts the site and the runner.

If the pull changed `install.sh` or anything in `deploy/`, run `./install.sh` instead. It deploys too.

On a network that needs a proxy, set `https_proxy`/`http_proxy` before running either script. pacman, pip and podman use them. If your shell profile sets a proxy that the current network doesn't have, unset it for one run: `env -u https_proxy -u HTTPS_PROXY -u http_proxy -u HTTP_PROXY ./deploy.sh`.

### Logs and status

```
systemctl status gunicorn shadertree-runner shadertree-backup.timer
systemctl --user status shadertree-nginx
journalctl -u shadertree-runner -f
```

### Backups

`~/shadertree-backups/` holds hourly copies of the database, plus one from every deploy. The newest 168 are kept (about 2 MB each). Copy them off the Deck now and then.

To restore one: `sudo systemctl stop gunicorn shadertree-runner`, copy the backup over `/var/www/shadertree/db.sqlite3`, delete `db.sqlite3-wal` and `db.sqlite3-shm` next to it, then start both services again.

To seed a fresh database with the shader prefix, default editor text and LED coordinates, export them from the live one in `/var/www/shadertree`:
```
venv/bin/python manage.py dumpdata shader_app.shaderprefix shader_app.defaultshadereditortext shader_app.ledcoordscfg --indent 1 -o singletons.json
```
Load them after `migrate` with `venv/bin/python manage.py loaddata singletons.json`.

## Development

### Setup

- **Windows:** `powershell -ExecutionPolicy Bypass -File setup-dev.ps1`. It checks for Visual Studio 2022 or newer with the C++ tools and vcpkg (listed in `reconstruct\.vsconfig`; if anything is missing it prints the install command). It sets `VCPKG_ROOT` to Visual Studio's bundled vcpkg unless you already have one, and creates `.venv` with Python 3.12.
- **Ubuntu 24.04 and Arch:** `./setup-dev.sh`. It installs the solver's dependencies with apt or pacman and creates `.venv`.

Both are safe to re-run. `.venv` has the site's requirements and the reconstruction tools' requirements. Activate it with `.venv\Scripts\activate` (Windows) or `source .venv/bin/activate` (Linux).

### Running the site locally

From `stweb/`, with `.venv` active:
```
python manage.py migrate
python manage.py createsuperuser
python manage.py runserver --insecure
```
The site is at http://127.0.0.1:8000/. `--insecure` serves the static files, which `runserver` otherwise skips because `DEBUG` is off. The editor preview works without the tree. Shaders only play if the runner is running.

Tests: `python manage.py test shader_app`.

### Building runshader

`make` in the repository root, on Linux with g++, the EGL/OpenGL headers (libglvnd) and libusb. It writes `shaderrunner/runshader` and copies it to `stweb/`. To try a shader without the LED boards, run it from a directory with a `ledcoords.dat` (the runner writes one): `./runshader --no-usb < shader.glsl`. The shader must include the prefix.

### VS Code

Open the repository root and install the recommended extensions (CMake Tools, C/C++, Python). Pick the `windows` or `linux` configure preset and a build preset (`windows-relwithdebinfo` for debugging). Ctrl+Shift+B lists build tasks for each configuration. The `ledreconstruct` launch configurations build the selected preset, then solve from a directory of cached observation JSONs. Python completion for `stweb/` uses `.venv`. On Windows, restart VS Code after the first `setup-dev.ps1` run so it sees `VCPKG_ROOT`.

## LED reconstruction

### Building the solver

`reconstruct/` builds `ledreconstruct` with CMake (3.22+) and a C++17 compiler. Dependencies: Ceres 2.2+, Eigen 3.4+, OpenCV 4.6+ (5.x works; FFmpeg is needed for video decode), jsoncpp and cxxopts 3.1+. Run the setup script for your platform first (see [Setup](#setup)).

**Windows**, from a Developer PowerShell:
```
cd reconstruct
cmake --preset windows
cmake --build --preset windows-release
```
vcpkg builds the dependencies from `vcpkg.json` on the first configure (slow; cached afterwards). The executable and its DLLs end up in `build\windows\Release\`. You can also open `reconstruct\` as a folder in Visual Studio. For a faster debuggable build use `cmake --build --preset windows-relwithdebinfo`. Debug works, but the solve is very slow.

**Ubuntu 24.04 and Arch:**
```
cd reconstruct
cmake --preset linux
cmake --build --preset linux -j
```
The executable is `build/linux/ledreconstruct`.

**SteamOS:** build inside an Arch container. Run `distrobox create --name arch --image archlinux:latest`, then `distrobox enter arch` and follow the Arch steps.

**Other platforms:** use vcpkg for the dependencies: `cmake --preset linux -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.

A build-only check runs on GitHub Actions for Windows, Ubuntu and Arch. Start it by hand under Actions > reconstruct > Run workflow.

### Calibrating the camera

The solver needs a calibration for the capture camera (`--calibration`). Make one with the tools in `reconstruct/tools`, with `.venv` active:

1. `python reconstruct/tools/make_charuco_target.py charuco.png`, and show the image full screen on a monitor.
2. Film it with the capture camera, at the zoom and resolution you'll use for the tree. Cover many angles and distances, including the corners of the frame.
3. `python reconstruct/tools/calibrate_charuco_from_video.py <videos> --output calib.json`

Keep `calib.json` with your captures, outside the repository.

### Capturing and solving

1. On the Deck: `sudo systemctl stop shadertree-runner`, then from the repository root run `(cd /var/www/shadertree && ./runshader) < shaderrunner/ledcodes.glsl`. Every LED blinks its address.
2. Film the tree from all around with the calibrated camera at 59.94 fps. 2025 used 156 clips.
3. On a PC: `ledreconstruct --videos <video dir> --dump-observations <video dir>/obs --calibration calib.json --output points.json --site site`. Reruns reuse the decoded observations in `obs/`.
4. In the admin, back up the `LedCoordsCfg` content, then paste in `site.ledcoords.json`. If the tree comes out upside down in the editor preview, rerun with `--flip`.
5. Stop `runshader` with Ctrl+C, then `sudo systemctl start shadertree-runner`.

## Credits

**TODO: authors and contributors.**

**TODO: third-party code, e.g. the USB driver (`shaderrunner/os_generic.h` carries its own license), and anything else to credit.**

## License

**TODO: license.**
