#!/bin/bash
# Deploys this checkout to the production dir and restarts the site and the runner.
# Run ./install.sh first, and again after every SteamOS update.
set -e
cd "$(dirname "$0")"
PROD_DIR=/var/www/shadertree

make

# --delete removes files that are gone from stweb/. Excluded paths exist only in production and are kept.
rsync -av --delete \
    --exclude='/venv/' \
    --exclude='__pycache__/' \
    --exclude='*.pyc' \
    --exclude='/db.sqlite3*' \
    --exclude='/secret_key.txt' \
    --exclude='/local.env' \
    --exclude='/static/' \
    --exclude='/backups' \
    --exclude='/*.dat' \
    stweb/ "$PROD_DIR/"

cd "$PROD_DIR"
# SteamOS updates can replace the system Python, which breaks the venv
if ! venv/bin/python -c 'import django' 2>/dev/null; then
    rm -rf venv
    python3 -m venv venv
fi
venv/bin/pip install -r requirements.txt
if [ -e db.sqlite3 ]; then
    venv/bin/python manage.py backup_db
fi
venv/bin/python manage.py migrate
venv/bin/python manage.py collectstatic --noinput
sudo systemctl restart gunicorn shadertree-runner
echo "Deployment completed successfully."
