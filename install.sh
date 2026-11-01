#!/bin/bash
# Sets up a Steam Deck to run shadertree, then deploys this checkout.
# Safe to re-run. Run it again after every SteamOS update: updates can remove what it puts in /etc.
# Usage: ./install.sh [--import-db PATH]   (PATH: a database or backup, used only if production has none)
set -e
cd "$(dirname "$0")"
PROD_DIR=/var/www/shadertree
IMPORT_DB=
if [ "$1" = "--import-db" ]; then
    IMPORT_DB=$2
fi

for tool in podman python3 rsync; do
    if ! command -v $tool >/dev/null; then
        echo "$tool is missing. SteamOS 3.9 includes it; on other systems install it first." >&2
        exit 1
    fi
done

# Building runshader needs a compiler and headers. SteamOS ships neither, and updates remove them.
if ! command -v g++ >/dev/null || [ ! -e /usr/include/stdio.h ] || [ ! -e /usr/include/EGL/egl.h ] || [ ! -e /usr/include/libusb-1.0/libusb.h ]; then
    # pacman runs as root, whose environment can carry its own proxy settings. Give it exactly ours.
    SUDO_NET=(sudo env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY -u no_proxy -u NO_PROXY)
    for var in http_proxy https_proxy HTTP_PROXY HTTPS_PROXY no_proxy NO_PROXY; do
        if [ -n "${!var}" ]; then
            SUDO_NET+=("$var=${!var}")
        fi
    done
    sudo steamos-readonly disable
    sudo pacman-key --init
    sudo pacman-key --populate
    "${SUDO_NET[@]}" pacman -Sy --noconfirm --needed base-devel
    # Reinstalled, not skipped, because SteamOS strips their headers
    "${SUDO_NET[@]}" pacman -S --noconfirm glibc linux-api-headers libglvnd libusb
    sudo steamos-readonly enable
fi

sudo mkdir -p "$PROD_DIR"
sudo chown "$USER:" "$PROD_DIR"

# /var is only 230 MB on the Deck, so backups live in /home
mkdir -p ~/shadertree-backups
if [ -d "$PROD_DIR/backups" ] && [ ! -L "$PROD_DIR/backups" ]; then
    mv "$PROD_DIR"/backups/* ~/shadertree-backups/ 2>/dev/null || true
    rmdir "$PROD_DIR/backups"
fi
[ -L "$PROD_DIR/backups" ] || ln -s ~/shadertree-backups "$PROD_DIR/backups"

if [ ! -f "$PROD_DIR/local.env" ]; then
    cat > "$PROD_DIR/local.env" <<'ENV'
# Hostnames and IPs the site is reached by, comma-separated. localhost always works.
SHADERTREE_ALLOWED_HOSTS=
# 1 runs the runner without the LED boards attached
SHADERTREE_NO_USB=0
ENV
fi

if [ -n "$IMPORT_DB" ]; then
    if [ -e "$PROD_DIR/db.sqlite3" ]; then
        echo "$PROD_DIR/db.sqlite3 already exists. Not importing over it." >&2
        exit 1
    fi
    cp "$IMPORT_DB" "$PROD_DIR/db.sqlite3"
fi

# Everything from here to deploy.sh goes in /etc, which SteamOS updates may reset
sudo install -m 644 shaderrunner/99-x32-ws2812-usbhs-breakout.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger
# nginx runs in a rootless container and needs port 80
echo "net.ipv4.ip_unprivileged_port_start=80" | sudo tee /etc/sysctl.d/50-shadertree.conf >/dev/null
sudo sysctl -q -p /etc/sysctl.d/50-shadertree.conf
sudo install -m 644 deploy/gunicorn.service deploy/shadertree-runner.service \
    deploy/shadertree-backup.service deploy/shadertree-backup.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable gunicorn shadertree-runner shadertree-backup.timer
# Starts this user's containers at boot without a login
sudo loginctl enable-linger "$USER"

mkdir -p ~/.config/containers/systemd ~/.config/shadertree
install -m 644 deploy/shadertree-nginx.container ~/.config/containers/systemd/
install -m 644 deploy/nginx-shadertree.conf ~/.config/shadertree/nginx.conf
systemctl --user daemon-reload

./deploy.sh
sudo systemctl start shadertree-backup.timer
systemctl --user restart shadertree-nginx

if grep -q '^SHADERTREE_ALLOWED_HOSTS=$' "$PROD_DIR/local.env"; then
    echo "Next: set SHADERTREE_ALLOWED_HOSTS in $PROD_DIR/local.env, then run: sudo systemctl restart gunicorn"
fi
