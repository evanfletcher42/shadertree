#!/bin/bash

read -p "This is going to completely blow away anything in the production dir, including databases. Confirm? [Y/N]: " confirm
if [[ "$confirm" != "Y" ]]; then
  echo "Operation canceled."
  exit 1
fi


DEV_DIR=./stweb/
PROD_DIR=/var/www/shadertree/

# initial copy of everything except the venv
# note: includes the db
rsync -av --exclude='venv' $DEV_DIR $PROD_DIR


pushd $PROD_DIR

# setup venv
python3 -m venv venv
source venv/bin/activate
pip install --upgrade pip
pip install -r requirements.txt

deactivate
popd
echo "Done"
