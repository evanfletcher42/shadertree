# shadertree

## Running the site on the Deck

The site (gunicorn behind nginx), the shader runner and an hourly database backup run as systemd services from `/var/www/shadertree/`. The unit files and the nginx config are in `deploy/`; they assume the `deck` user.

### First-time setup

1. Install the build dependencies (`shaderrunner/setup_notes.txt`) and nginx, then `make install_udev_rules`.
2. `./setup_deploy.sh` creates `/var/www/shadertree/` and its venv. It copies `stweb/` including the dev database, so it asks for the hostname first.
3. Install and start the services:
   ```
   sudo cp deploy/*.service deploy/*.timer /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now gunicorn shadertree-runner shadertree-backup.timer
   ```
   Then `sudo systemctl edit gunicorn` and add the names the site is reached by (anything else gets a 400):
   ```
   [Service]
   Environment=SHADERTREE_ALLOWED_HOSTS=shadertree.example.com,10.0.0.5
   ```
4. Copy `deploy/nginx-shadertree.conf` to `/etc/nginx/sites-enabled/shadertree.conf` and set `server_name` to the same names as `SHADERTREE_ALLOWED_HOSTS`. Make sure `/etc/nginx/nginx.conf` has `include /etc/nginx/sites-enabled/*;` in its `http` block. Then `sudo nginx -t && sudo systemctl reload nginx`.

### Deploying

`./deploy.sh` builds `runshader`, syncs `stweb/` to `/var/www/shadertree/` (deleting files that are no longer in the repo), backs up the database, installs requirements, migrates, collects static files and restarts the site and the runner.

Logs: `journalctl -u shadertree-runner -f` (or `-u gunicorn`).

Without the LED boards attached, run `sudo systemctl edit shadertree-runner` and add:
```
[Service]
Environment=SHADERTREE_NO_USB=1
```

### Backups

`/var/www/shadertree/backups/` holds hourly copies of the database, plus one from every deploy. The newest 168 are kept. Copy them off the Deck now and then.

To restore one: `sudo systemctl stop gunicorn shadertree-runner`, copy the backup over `db.sqlite3`, delete `db.sqlite3-wal` and `db.sqlite3-shm`, then start both services again.

To seed a fresh database with the shader prefix, default editor text and LED coordinates, export them from the live one:
```
venv/bin/python manage.py dumpdata shader_app.shaderprefix shader_app.defaultshadereditortext shader_app.ledcoordscfg --indent 1 -o singletons.json
```
Load them after `migrate` with `manage.py loaddata singletons.json`.

## Building the reconstruction solver

`reconstruct/` builds `ledreconstruct` with CMake (3.22+) and a C++17 compiler. Dependencies: Ceres 2.2+, Eigen 3.4+, OpenCV 4.6+ (5.x works; FFmpeg is needed for video decode), jsoncpp and cxxopts 3.1+.

### Windows

Run `powershell -ExecutionPolicy Bypass -File setup-dev.ps1` once. It checks for Visual Studio 2022 or newer with the C++ tools and vcpkg (listed in `reconstruct\.vsconfig`; if anything is missing it prints the install command), sets `VCPKG_ROOT` to Visual Studio's bundled vcpkg unless you already have one, and creates `.venv` (Python 3.12) for the web side and the reconstruction dev tools. Then, from a Developer PowerShell:

```
cd reconstruct
cmake --preset windows
cmake --build --preset windows-release
```

vcpkg builds the dependencies from `vcpkg.json` on the first configure (slow; cached afterwards). The executable and its DLLs end up in `build\windows\Release\`. You can also open `reconstruct\` as a folder in Visual Studio.

For a faster debuggable build use `cmake --build --preset windows-relwithdebinfo` (Debug works but the solve is very slow).

### Ubuntu 24.04 and Arch

Run `./setup-dev.sh` once. It installs the dependencies with apt or pacman and creates `.venv`. Then:

```
cd reconstruct
cmake --preset linux
cmake --build --preset linux -j
```

### SteamOS

Build inside an Arch container: `distrobox create --name arch --image archlinux:latest`, then `distrobox enter arch` and follow the Arch steps.

### VS Code

Open the repository root and install the recommended extensions (CMake Tools, C/C++, Python). Pick the `windows` or `linux` configure preset and a build preset (`windows-relwithdebinfo` for debugging). Ctrl+Shift+B lists build tasks for each configuration; the `ledreconstruct` launch configurations build the selected preset, then solve from a directory of cached observation JSONs. Python completion for `stweb/` uses the `.venv` from the setup script. On Windows, restart VS Code after the first `setup-dev.ps1` run so it sees `VCPKG_ROOT`.

### Other platforms

Use vcpkg for the dependencies: `cmake --preset linux -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.

## Calibrating the camera

The solver needs a calibration for the capture camera (`--calibration`). Make one with the dev tools in `reconstruct/tools` (from the `.venv`):

1. `python reconstruct/tools/make_charuco_target.py charuco.png` and show it full screen on a monitor.
2. Film it with the capture camera at the capture zoom and resolution, from many angles and distances, covering the frame corners.
3. `python reconstruct/tools/calibrate_charuco_from_video.py <videos> --output calib.json`

Keep `calib.json` with your captures, outside the repository.

## Reconstructing the LED positions

1. On the Deck: `sudo systemctl stop shadertree-runner`, then from the repository root run `(cd /var/www/shadertree && ./runshader) < shaderrunner/ledcodes.glsl`. Every LED blinks its address.
2. Film the tree from all around with the calibrated camera at 59.94 fps (2025 used 156 clips).
3. On a PC: `ledreconstruct --videos <video dir> --dump-observations <video dir>/obs --calibration calib.json --output points.json --site site`. Reruns reuse the decoded observations in `obs/`.
4. In the admin, back up the `LedCoordsCfg` content, then paste in `site.ledcoords.json`. If the tree comes out upside down in the editor preview, rerun with `--flip`.
5. Stop `runshader` with Ctrl+C and `sudo systemctl start shadertree-runner`.
