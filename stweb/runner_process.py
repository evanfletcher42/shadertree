import os
import django
from django.db import transaction, IntegrityError, OperationalError, connection
import sys
import subprocess
import random
import time
import struct


def write_floats_file(data, f_path):
    data_packed = struct.pack(f'{len(data)}f', *data)
    with open(f_path, 'wb') as f:
        f.write(data_packed)

def init_django():
    # sys.path.append(os.path.normpath('../stweb/'))
    os.environ.setdefault('DJANGO_SETTINGS_MODULE', 'shadertree.settings')
    django.setup()


def runshader_interruptible(shader_text, timeout_seconds, should_interrupt, poll_seconds=0.1):
    p = subprocess.Popen(
        ["./runshader"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    # send program and close stdin
    p.stdin.write(shader_text.encode())
    p.stdin.close()

    start = time.monotonic()

    while True:
        rc = p.poll()
        if rc is not None:
            stdout = p.stdout.read().decode(errors="replace")
            stderr = p.stderr.read().decode(errors="replace")
            return {"interrupted": False, "timeout": False, "return_code": rc, "stdout": stdout, "stderr": stderr}

        if should_interrupt():
            p.terminate()
            try:
                p.wait(timeout=1)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
            stdout = p.stdout.read().decode(errors="replace")
            stderr = p.stderr.read().decode(errors="replace")
            return {"interrupted": True, "timeout": False, "return_code": p.returncode, "stdout": stdout, "stderr": stderr}

        if (time.monotonic() - start) >= timeout_seconds:
            p.terminate()
            try:
                p.wait(timeout=1)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
            stdout = p.stdout.read().decode(errors="replace")
            stderr = p.stderr.read().decode(errors="replace")
            return {"interrupted": False, "timeout": True, "return_code": p.returncode, "stdout": stdout, "stderr": stderr}

        time.sleep(poll_seconds)


def sqlite_enable_wal():
    if connection.vendor != "sqlite":
        return
    with connection.cursor() as c:
        c.execute("PRAGMA journal_mode=WAL;")
        c.execute("PRAGMA synchronous=NORMAL;")


def main():
    init_django()
    sqlite_enable_wal()

    from shader_app.models import Shader, ShaderVersion, ShaderPrefix, LedCoordsCfg
    from shader_app.playback import get_ctrl_values, consume_override_if_new

    last_id = None
    last_seen_nonce = 0

    while True:
        # --- load prefix (your existing logic) ---
        try:
            pfx_obj = ShaderPrefix.objects.first()
            prefix_text = pfx_obj.content if pfx_obj else ""
        except ShaderPrefix.DoesNotExist:
            print("No shader prefix configured")
            time.sleep(1)
            continue

        # --- if someone requested a shader, consume it and play it now ---
        try:
            forced_shader, last_seen_nonce = consume_override_if_new(last_seen_nonce)
        except OperationalError as e:
            # sqlite transient lock; back off a hair and retry loop
            time.sleep(0.05)
            continue

        if forced_shader is not None:
            shader = forced_shader
        else:
            # --- your existing random selection logic ---
            qs = Shader.objects.filter(is_deleted=False, is_playable=True)
            n_shaders = qs.count()
            if n_shaders == 0:
                print("No shaders available")
                time.sleep(1)
                continue

            if n_shaders == 1:
                shader = qs.first()
            else:
                if last_id:
                    qs = qs.exclude(id=last_id)
                n_shaders = qs.count()
                shader = qs.all()[random.randint(0, n_shaders - 1)]
                last_id = shader.id

        # --- load shader text (your existing logic) ---
        latest_shader_version = shader.versions.order_by('-created_at').first()
        if not latest_shader_version:
            print("Failed to load shader text for shader:", shader)
            continue
        shader_text = latest_shader_version.content

        # --- LED config + file write (your existing logic) ---
        coords_obj = LedCoordsCfg.objects.first()
        coords_list = coords_obj.content
        write_floats_file(coords_list, "ledcoords.dat")

        # --- mark now playing (your existing logic) ---
        Shader.objects.filter(is_playing=True).update(is_playing=False)
        shader.is_playing = True
        shader.save()

        # --- run shader, interrupt if nonce changes ---
        shadertime = 30
        program_text = prefix_text + "\n" + shader_text

        print("Running:", shader)

        def should_interrupt():
            nonlocal last_seen_nonce
            shader_id, nonce = get_ctrl_values()

            # Only interrupt if a shader is actually pending AND nonce changed
            if shader_id is None:
                # Still update our marker if we missed an update (optional but helpful)
                last_seen_nonce = max(last_seen_nonce, nonce)
                return False

            return nonce != last_seen_nonce

        result = runshader_interruptible(program_text, shadertime, should_interrupt, poll_seconds=0.1)
        if not result["interrupted"] and not result["timeout"]:
            print(f"runshader exited early (rc={result['return_code']}): {shader}\n{result['stderr']}", flush=True)
            time.sleep(1)


if __name__ == "__main__":
    main()
