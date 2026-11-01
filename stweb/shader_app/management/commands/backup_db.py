import sqlite3
from contextlib import closing
from datetime import datetime

from django.conf import settings
from django.core.management.base import BaseCommand
from django.db import connection

BACKUP_DIR = settings.BASE_DIR / "backups"
KEEP = 168


class Command(BaseCommand):
    help = f"Copy the database to {BACKUP_DIR}, keeping the newest {KEEP} copies. Safe while the site is running."

    def handle(self, *args, **options):
        BACKUP_DIR.mkdir(exist_ok=True)
        path = BACKUP_DIR / f"db-{datetime.now():%Y%m%d-%H%M%S}.sqlite3"
        connection.ensure_connection()
        with closing(sqlite3.connect(path)) as dst:
            connection.connection.backup(dst)
        for old in sorted(BACKUP_DIR.glob("db-*.sqlite3"))[:-KEEP]:
            old.unlink()
        self.stdout.write(str(path))
