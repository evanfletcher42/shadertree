#!/bin/bash
# Deploys this checkout to the production dir and restarts the site and the runner.
set -e
cd "$(dirname "$0")"
PROD_DIR=/var/www/shadertree/

make

# --delete removes files that are gone from stweb/. Excluded paths exist only in production and are kept.
rsync -av --delete \
    --exclude='/venv/' \
    --exclude='__pycache__/' \
    --exclude='*.pyc' \
    --exclude='/db.sqlite3*' \
    --exclude='/secret_key.txt' \
    --exclude='/static/' \
    --exclude='/backups/' \
    --exclude='/*.dat' \
    --exclude='/shadertree.sock' \
    stweb/ "$PROD_DIR"

cd "$PROD_DIR"
venv/bin/python manage.py backup_db
venv/bin/pip install -r requirements.txt
venv/bin/python manage.py migrate
venv/bin/python manage.py collectstatic --noinput
sudo systemctl restart gunicorn shadertree-runner
echo "Deployment completed successfully."
