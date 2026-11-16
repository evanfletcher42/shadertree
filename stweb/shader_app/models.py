import random
from collections import Counter
from datetime import timedelta
from django.conf import settings
from django.db import models
from django.utils.crypto import get_random_string
from django.core.exceptions import ValidationError
from django.db.models import Q

class Shader(models.Model):
    """
    Shader metadata. Includes its title, author, unique identifier (used for URL), and more.
    """
    ident = models.CharField(max_length=10, unique=True, blank=True)
    created_at = models.DateTimeField(auto_now_add=True)
    author = models.CharField(max_length=255)
    title = models.CharField(max_length=255)
    is_playable = models.BooleanField(default=False)  # whether this shader should run on the tree
    is_deleted = models.BooleanField(default=False)  # soft deletion flag
    is_playing = models.BooleanField(default=False)
    compile_failures = models.PositiveIntegerField(default=0)  # consecutive failed compiles on the tree
    compile_error = models.TextField(blank=True)  # last tree compile error, shown in the editor

    def save(self, *args, **kwargs):
        if not self.ident:
            self.ident = self.gen_unique_code()
        super().save(*args, **kwargs)

    def gen_unique_code(self):
        length = 10
        c = get_random_string(length)
        while Shader.objects.filter(ident=c).exists():
            c = get_random_string(length)
        return c
    
    class Meta:
        constraints = [
            # Only one shader can run on the tree at a time
            models.UniqueConstraint(
                fields=['is_playing'],
                condition=Q(is_playing=True),
                name='unique_now_playing'
            )
        ]

    def __str__(self):
        return f"Shader: {self.title} ({self.ident})"
    
class ShaderVersion(models.Model): 
    """
    The actual versioned text content of a shader. We store all versions of the shaders themselves.
    """
    shader = models.ForeignKey(Shader, on_delete=models.CASCADE, related_name="versions")
    created_at = models.DateTimeField(auto_now_add=True)
    ip_addr = models.GenericIPAddressField(null=True, blank=True)  # IP addr of the host that made this edited version of the shader
    content = models.TextField()  # Actual text content of the shader itself

    def __str__(self):
        return f"ShaderVersion at {self.created_at} for Shader {self.shader.ident}"

class ShaderPrefix(models.Model):
    """
    Contains the prefix that every shader is compiled with.
    """
    content = models.TextField("prefix", default=
        "#version 300 es\n"
        "layout(location = 0) in vec3 iPos;  // LED 3D position: +y up, bottom center ~(0,-1,0), tip ~(0,1,0)\n"
        "layout(location = 1) in uint iIdx;  // LED address in the string (raw hardware addresses; not contiguous)\n"
        "uniform float iTime;                // seconds since the shader started\n"
        "uniform int iFrame;                 // frame counter, from 0\n"
        "out vec3 outColor;                  // output: RGB color, 0 to 1"
        )

    def save(self, *args, **kwargs):
        if not self.pk and ShaderPrefix.objects.exists():
            raise ValidationError("There can only be one ShaderPrefix instance")
        super().save(*args, **kwargs)

    @staticmethod
    def program_prefix():
        pfx_obj = ShaderPrefix.objects.first()
        return (pfx_obj.content if pfx_obj else "") + "\n"

    def __str__(self):
        return "Shader Prefix Configuration"

class DefaultShaderEditorText(models.Model):
    """
    Contains the default content of the editor block, when someone makes a new shader.
    """
    content = models.TextField("text", default=
        "void main() {\n"
        "    outColor = vec3(\n"
        "        sin(iPos.x + iTime),\n"
        "        sin(iPos.y + iTime*2.0f),\n"
        "        sin(iPos.z + iTime*4.0f)\n"
        "    );\n"
        "    \n"
        "    outColor = 0.5f + 0.5f*outColor;\n"
        "}\n"
        )
    
    def save(self, *args, **kwargs):
        if not self.pk and DefaultShaderEditorText.objects.exists():
            raise ValidationError("There can only be one DefaultShaderEditorText instance")
        super().save(*args, **kwargs)
    
    def __str__(self):
        return "Default Shader Editor Text Configuration"

class LedCoordsCfg(models.Model):
    """
    Contains the positions of each LED in the model, as a json array.
    """
    content = models.JSONField()

    def led_count(self):
        c = self.content
        counts = Counter(tuple(c[i:i + 3]) for i in range(0, len(c) - 2, 3))
        return sum(1 for n in counts.values() if n == 1)
    
    def save(self, *args, **kwargs):
        if not self.pk and LedCoordsCfg.objects.exists():
            raise ValidationError("There can only be one LedCoordsCfg instance")
        super().save(*args, **kwargs)
    
    def __str__(self):
        return "LED Coordinates Configuration"

class BannedIP(models.Model):
    ip_address = models.GenericIPAddressField(unique=True)
    banned_at = models.DateTimeField(auto_now_add=True)
    reason = models.TextField(blank=True, null=True)

    def __str__(self):
        return self.ip_address

class PlayNowRequest(models.Model):
    created_at = models.DateTimeField(auto_now_add=True)
    ip_addr = models.GenericIPAddressField(null=True, blank=True)
    shader = models.ForeignKey(Shader, null=True, on_delete=models.SET_NULL)
    accepted = models.BooleanField()  # false if rejected by the cooldown

class PlaybackControl(models.Model):
    # Singleton (always pk=1)
    force_shader = models.ForeignKey(
        "Shader", null=True, blank=True, on_delete=models.SET_NULL
    )
    updated_at = models.DateTimeField(auto_now=True)

    # Playlist state. Covers the rest of this cycle + all of the next, s.t. every playable shader has a next play time.
    # Only the runner writes these.
    queue = models.JSONField(default=list)  # shader ids still to play this cycle, in order
    next_cycle = models.JSONField(default=list)  # every playable shader id, in next cycle's order
    current = models.BigIntegerField(null=True, blank=True)  # shader id the runner is on
    started_at = models.DateTimeField(null=True, blank=True)  # start of the current shader; null when the runner is stopped

    play_now_cooldown = models.PositiveIntegerField(default=60, help_text="Seconds an IP address must wait between Play Now requests.")
    play_now_cap = models.PositiveIntegerField(default=10, help_text="Accepted Play Now requests allowed per IP address in the cap window. 0 turns the cap off.")
    play_now_cap_minutes = models.PositiveIntegerField(default=60, help_text="Length of the cap window, in minutes.")
    auto_ban_rejections = models.PositiveIntegerField(default=20, help_text="Ban an IP address with more than this many rejected Play Now requests in the ban window. 0 turns auto-ban off.")
    auto_ban_hours = models.PositiveIntegerField(default=24, help_text="Length of the ban window, in hours.")

    def __str__(self):
        return "Playback Control"

    @staticmethod
    def get():
        return PlaybackControl.objects.get_or_create(pk=1)[0]

    def sync(self, playable_ids):
        """Sync the playlist with the current set of playable shaders. New shaders are inserted at random positions in this cycle and the next."""
        playable = set(playable_ids)
        self.queue = [i for i in self.queue if i in playable]
        self.next_cycle = [i for i in self.next_cycle if i in playable]
        for i in playable - set(self.next_cycle):
            if i not in self.queue:
                self.queue.insert(random.randint(0, len(self.queue)), i)
            self.next_cycle.insert(random.randint(0, len(self.next_cycle)), i)
        self.avoid_repeat()

    def take_next(self):
        """Advance to the next shader id, starting the next cycle when this one is done. Returns None if nothing is playable."""
        if not self.queue:
            self.queue = self.next_cycle
            self.next_cycle = random.sample(self.queue, len(self.queue))
        self.current = self.queue.pop(0) if self.queue else None
        self.avoid_repeat()
        return self.current

    def play_next(self, shader_id):
        if shader_id in self.queue:
            self.queue.remove(shader_id)
        self.queue.insert(0, shader_id)
        self.avoid_repeat()

    def avoid_repeat(self):
        # the next cycle shouldn't start with the shader that ends this one
        last = self.queue[-1] if self.queue else self.current
        if len(self.next_cycle) > 1 and self.next_cycle[0] == last:
            self.next_cycle[0], self.next_cycle[1] = self.next_cycle[1], self.next_cycle[0]

    def upcoming(self):
        """Returns a list of (shader, estimated start time) through the end of the next cycle, soonest first.
        Most shaders appear twice. Start times are None while the runner is stopped."""
        ids = self.queue + self.next_cycle
        shaders = Shader.objects.in_bulk(ids)
        duration = timedelta(seconds=settings.SHADER_PLAY_SECONDS)
        return [(shaders[i], self.started_at + duration * (n + 1) if self.started_at else None)
                for n, i in enumerate(ids) if i in shaders]