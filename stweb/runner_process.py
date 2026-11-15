import os
import signal
import struct
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path

import django

os.environ.setdefault('DJANGO_SETTINGS_MODULE', 'shadertree.settings')
django.setup()

from django.conf import settings
from django.db import OperationalError, close_old_connections, connection, transaction
from django.db.models import F
from django.utils import timezone
from shader_app.models import LedCoordsCfg, PlaybackControl, Shader, ShaderPrefix

BASE_DIR = Path(__file__).resolve().parent
RUNSHADER = BASE_DIR / "runshader"
COMPILED_LINE = "Shader compiled successfully"
COMPILE_FAILED_CODES = (1, 3)  # runshader exit codes for user shader compile/link failures
POLL_SECONDS = 0.1

_last_coords = None


def sqlite_enable_wal():
    if connection.vendor != "sqlite":
        return
    with connection.cursor() as c:
        c.execute("PRAGMA journal_mode=WAL;")
        c.execute("PRAGMA synchronous=NORMAL;")


def write_coords():
    """Write LED coordinates to ledcoords.dat for runshader. Returns False if no coordinates are configured."""
    global _last_coords
    cfg = LedCoordsCfg.objects.first()
    if cfg is None:
        return False
    if cfg.content != _last_coords:
        with open(BASE_DIR / "ledcoords.dat", 'wb') as f:
            f.write(struct.pack(f'{len(cfg.content)}f', *cfg.content))
        _last_coords = cfg.content
    return True


def playable_ids():
    return list(Shader.objects.filter(is_deleted=False, is_playable=True).values_list("id", flat=True))


def take_play_now():
    """Clear a pending play-now request and return its shader id."""
    shader_id = PlaybackControl.objects.filter(pk=1).values_list("force_shader_id", flat=True).first()
    if shader_id is None:
        return None
    # only clear the request we read; a newer one is picked up on the next poll
    if not PlaybackControl.objects.filter(pk=1, force_shader_id=shader_id).update(force_shader=None):
        return None
    return shader_id


def refresh(ctrl):
    """Sync the playlist with the playable shaders and apply any play-now. Returns True if a play-now request was applied."""
    close_old_connections()
    playable = playable_ids()
    before = (list(ctrl.queue), list(ctrl.next_cycle))
    ctrl.sync(playable)
    forced = take_play_now()
    applied = forced in playable
    if applied:
        ctrl.play_next(forced)
    if (ctrl.queue, ctrl.next_cycle) != before:
        ctrl.save(update_fields=["queue", "next_cycle"])
    return applied


def set_now_playing(shader_id):
    with transaction.atomic():
        Shader.objects.filter(is_playing=True).update(is_playing=False)
        if shader_id is not None:
            Shader.objects.filter(pk=shader_id).update(is_playing=True)


def record_compile(shader_id, error):
    """Track consecutive compile failures and take the shader off the tree after too many."""
    shaders = Shader.objects.filter(pk=shader_id)
    if error is None:
        shaders.update(compile_failures=0, compile_error="")
        return
    shaders.update(compile_failures=F("compile_failures") + 1, compile_error=error)
    shaders.filter(compile_failures__gte=settings.SHADER_MAX_COMPILE_FAILURES).update(is_playable=False)


def stop_process(p):
    p.terminate()
    try:
        p.wait(timeout=1)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()


def run_shader(program_text, on_compiled, should_stop):
    """Run a shader in runshader for one playlist slot. Returns a tuple: (compiled, stopped_by_runner, return_code, stderr_tail)"""
    args = [str(RUNSHADER)] + (["--no-usb"] if settings.RUNSHADER_NO_USB else [])
    p = subprocess.Popen(args, cwd=BASE_DIR, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, errors="replace")

    # Read stdout and stderr continuously. Otherwise per-frame GL error spam can fill a pipe and block runshader.
    compiled = threading.Event()
    stderr_tail = deque(maxlen=200)

    def read_stdout():
        for line in p.stdout:
            if line.rstrip() == COMPILED_LINE:
                compiled.set()

    readers = [threading.Thread(target=read_stdout, daemon=True),
               threading.Thread(target=stderr_tail.extend, args=(p.stderr,), daemon=True)]
    for t in readers:
        t.start()

    stopped = False
    try:
        try:
            p.stdin.write(program_text)
            p.stdin.close()
        except BrokenPipeError:
            pass

        # Deadline resets once the shader compiles. Until then, it doubles as a compile timeout.
        deadline = time.monotonic() + settings.SHADER_PLAY_SECONDS
        started = False
        while p.poll() is None:
            if compiled.is_set() and not started:
                started = True
                on_compiled()
                deadline = time.monotonic() + settings.SHADER_PLAY_SECONDS
            if time.monotonic() >= deadline or should_stop():
                stopped = True
                break
            time.sleep(POLL_SECONDS)
    finally:
        if p.poll() is None:
            stop_process(p)
        for t in readers:
            t.join()

    return compiled.is_set(), stopped, p.returncode, "".join(stderr_tail)


def play_one(ctrl):

    refresh(ctrl)
    shader_id = ctrl.take_next()
    if shader_id is None:
        print("No shaders available")
        time.sleep(1)
        return
    ctrl.started_at = timezone.now()
    ctrl.save(update_fields=["queue", "next_cycle", "current", "started_at"])

    shader = Shader.objects.filter(pk=shader_id).first()
    version = shader.versions.order_by('-created_at').first() if shader else None
    if version is None:
        print("Failed to load shader text for shader id:", shader_id)
        time.sleep(1)
        return

    if not write_coords():
        print("No LED coordinates configured")
        time.sleep(1)
        return

    print("Running:", shader)

    def on_compiled():
        ctrl.started_at = timezone.now()
        ctrl.save(update_fields=["started_at"])
        set_now_playing(shader.id)

    def should_stop():
        # Stop if a play-now was applied or this shader is no longer playable
        return refresh(ctrl) or shader.id not in ctrl.next_cycle

    program_text = ShaderPrefix.program_prefix() + version.content
    compiled, stopped, return_code, stderr = run_shader(program_text, on_compiled, should_stop)

    if return_code in COMPILE_FAILED_CODES:
        record_compile(shader.id, stderr)
    elif compiled:
        record_compile(shader.id, None)

    if not stopped:
        set_now_playing(None)
        print(f"runshader exited early (rc={return_code}): {shader}\n{stderr}")
        time.sleep(1)


def main():
    sys.stdout.reconfigure(line_buffering=True)
    sqlite_enable_wal()

    # run the finally below on SIGTERM too
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    ctrl = PlaybackControl.get()
    set_now_playing(None)
    try:
        while True:
            try:
                play_one(ctrl)
            except OperationalError as e:
                print("Database error:", e)
                time.sleep(1)
                ctrl.refresh_from_db()
    finally:
        set_now_playing(None)
        PlaybackControl.objects.filter(pk=1).update(started_at=None)


if __name__ == "__main__":
    main()
