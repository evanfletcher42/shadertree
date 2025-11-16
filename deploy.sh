#!/bin/bash

DEV_DIR=./stweb/
PROD_DIR=/var/www/shadertree/
VENV_DIR=$PROD_DIR/venv
SERVICE_NAME=gunicorn
DB_FILE="db.sqlite3"  # Change this if your database file has a different name or path

echo "Starting deployment..."

# Step 1: Sync files from development to production, excluding venv, pyc files, __pycache__, and the database file
echo "Syncing files from $DEV_DIR to $PROD_DIR..."
rsync -av \
    --exclude='venv' \
    --exclude='*.pyc' \
    --exclude='__pycache__' \
    --exclude="$DB_FILE" \
    "$DEV_DIR" "$PROD_DIR"

# Step 2: Activate virtual environment
echo "Activating virtual environment..."
source "$VENV_DIR/bin/activate"

# Step 3: Navigate to production directory
pushd "$PROD_DIR" || { echo "Failed to navigate to $PROD_DIR"; exit 1; }

# Step 4: Upgrade pip and install dependencies
echo "Upgrading pip and installing dependencies..."
#pip install --upgrade pip
#pip install -r requirements.txt

# Step 5: Apply database migrations
echo "Applying database migrations..."
python manage.py migrate

# Step 6: Collect static files
echo "Collecting static files..."
python manage.py collectstatic --noinput

# Step 7: Deactivate virtual environment
deactivate

# Step 8: Restart Gunicorn service
echo "Restarting Gunicorn service..."
sudo systemctl restart "$SERVICE_NAME"

popd
echo "Deployment completed successfully."
