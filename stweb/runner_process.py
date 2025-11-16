import os
import django
from django.db import transaction, IntegrityError
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

def runshader_with_timeout(shader_text, timeout_seconds=300):
    run_result = {
        "quit_before_timeout": True,
        "sigterm": False,
        "sigkill": False,
        "stdout": None,
        "stderr": None,
        "return_code": None,
    }


    p = subprocess.Popen( ["./runshader", "/dev/ttyACM1"],
                         stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)

    try:
        stdout, stderr = p.communicate(input=shader_text.encode(), timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        run_result["quit_before_timeout"] = False
        run_result["sigterm"] = True

        # Send SIGTERM
        p.terminate()
        try:
            # Wait up to 1 second for process to exit
            stdout, stderr = p.communicate(timeout=1)
        except subprocess.TimeoutExpired:
            # Send SIGKILL
            run_result["sigkill"] = True
            p.kill()
            # Wait for process to exit and collect output
            stdout, stderr = p.communicate()

    run_result["stdout"] = stdout.decode()
    run_result["stderr"] = stderr.decode()

    run_result["return_code"] = p.returncode

    return run_result

def main():
    init_django()

    from shader_app.models import Shader, ShaderVersion, ShaderPrefix, LedCoordsCfg  # TODO: ShaderResult
    
    last_id = None

    while True:
        # get prefix
        try:
            pfx_obj = ShaderPrefix.objects.first()
            prefix_text = pfx_obj.content if pfx_obj else ""
        except ShaderPrefix.DoesNotExist:
            # try again later
            print("No shader prefix configured")
            time.sleep(1)
            continue

        # pick which shader we're running
        qs = Shader.objects.filter(is_deleted=False, is_playable=True)
        n_shaders = qs.count()

        if n_shaders == 0:
            # try again later
            print("No shaders available")
            time.sleep(1)
            continue

        if n_shaders == 1:
            shader = qs.first()
        else:
            if last_id:
                qs = qs.exclude(id=last_id)  # don't pick the same one twice in a row        
            n_shaders = qs.count()
            shader = qs.all()[random.randint(0, n_shaders - 1)]
            last_id = shader.id

        # get shader text
        try:
            latest_shader_version = shader.versions.order_by('-created_at').first()
            shader_text = latest_shader_version.content
        except ShaderVersion.DoesNotExist:
            print("Failed to load shader text for shader:", shader)
            continue

        # get LED config
        coords_obj = LedCoordsCfg.objects.first()
        coords_list = coords_obj.content

        # to file
        try:
            write_floats_file(coords_list, "ledcoords.dat")
        except Exception as e:
            print("Error writing LED coordinates to file:", e)
            return

        # Mark shader as "Now Playing"
        try:
            with transaction.atomic():
                # no longer playing any previous shader
                Shader.objects.filter(is_playing=True).update(is_playing=False)
                shader.is_playing = True
                shader.save()
        except IntegrityError:
            print("Failed to set is_playing=True due to a constraint error.")
            time.sleep(1)
            continue

        # run shader
        shadertime = 30  # TODO Load from db config
        program_text = prefix_text + '\n' + shader_text

        # TODO remove temporary thing
        print("Running:", shader)
        result = runshader_with_timeout(program_text, shadertime)

        # TODO log ShaderResult in db


if __name__ == "__main__":
    main()
