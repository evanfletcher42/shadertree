from django.db import close_old_connections, OperationalError
from shader_app.models import PlaybackControl, Shader

def get_ctrl_values():
    close_old_connections()
    row = (PlaybackControl.objects
           .filter(pk=1)
           .values("force_shader_id", "force_nonce")
           .first())
    if not row:
        return None, 0
    return row["force_shader_id"], row["force_nonce"]

def consume_override_if_new(last_seen_nonce: int):
    """
    Returns (shader_or_none, new_last_seen_nonce).
    IMPORTANT: advances nonce even if no shader is pending, to avoid repeated interrupts.
    """
    shader_id, nonce = get_ctrl_values()

    if nonce == last_seen_nonce:
        return None, last_seen_nonce

    # We've observed a new nonce; advance our local marker no matter what.
    last_seen_nonce = nonce

    if shader_id is None:
        # New nonce but nothing pending (already consumed/cleared)
        return None, last_seen_nonce

    # Consume (clear) pending shader
    close_old_connections()
    PlaybackControl.objects.filter(pk=1).update(force_shader=None)

    shader = Shader.objects.get(pk=shader_id)
    return shader, last_seen_nonce
