#!/bin/bash
# First-time setup of the production dir. Copies everything in stweb/, including the dev database.
set -e
cd "$(dirname "$0")"
PROD_DIR=/var/www/shadertree/

read -p "This overwrites everything in $PROD_DIR, including the database. Type this machine's hostname to continue: " answer
if [[ "$answer" != "$(hostname)" ]]; then
    echo "Operation canceled."
    exit 1
fi

rsync -av --exclude='/venv/' --exclude='/secret_key.txt' --exclude='/backups/' stweb/ "$PROD_DIR"
python3 -m venv "$PROD_DIR/venv"
"$PROD_DIR/venv/bin/pip" install --upgrade pip
"$PROD_DIR/venv/bin/pip" install -r "$PROD_DIR/requirements.txt"
echo "Done. Next: install the services and the nginx config (README.md)."
