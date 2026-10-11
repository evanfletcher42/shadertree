from collections import Counter
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
        "layout(location = 0) in vec3 iPos;\n"
        "layout(location = 1) in uint iIdx;\n"
        "uniform float iTime;\n"
        "uniform int iFrame;\n"
        "out vec3 outColor;"
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

class PlaybackControl(models.Model):
    # singleton row (pk=1)
    force_shader = models.ForeignKey(
        "Shader", null=True, blank=True, on_delete=models.SET_NULL
    )
    force_nonce = models.BigIntegerField(default=0)  # increments on each request
    updated_at = models.DateTimeField(auto_now=True)