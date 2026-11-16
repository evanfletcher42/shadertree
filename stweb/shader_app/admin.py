from django.contrib import admin, messages
from django.db.models import Count, Exists, OuterRef, Subquery
from django.db.models.functions import Coalesce
from django.shortcuts import redirect, get_object_or_404
from django.urls import path, reverse
from django.utils.html import format_html
from .models import Shader, ShaderVersion, ShaderPrefix, DefaultShaderEditorText, LedCoordsCfg, BannedIP, PlaybackControl, PlayNowRequest
from .utils import get_client_ip


def clone_as_latest(modeladmin, request, version):
    ShaderVersion.objects.create(shader=version.shader, ip_addr=get_client_ip(request), content=version.content)
    modeladmin.message_user(request, f"Version from {version.created_at:%Y-%m-%d %H:%M:%S} has been set as the latest version.", level=messages.SUCCESS)


class ShaderVersionInline(admin.TabularInline):
    """
    Inline admin interface for ShaderVersion within ShaderAdmin.
    """
    model = ShaderVersion
    extra = 0  # No extra blank forms
    readonly_fields = ('created_at', 'ip_addr', 'content', 'set_as_latest')
    fields = ('created_at', 'ip_addr', 'content', 'set_as_latest')
    can_delete = False  # Prevent deletion from inline
    ordering=['-created_at']

    def set_as_latest(self, instance):
        """
        Render a button/link to set this ShaderVersion as the latest.
        """
        return format_html(
            '<a class="button" href="{}">Set as Latest</a>',
            reverse(
                'admin:shader_app_shader_set_as_latest_version',
                args=[instance.shader.id, instance.id]
            )
        )
    set_as_latest.short_description = 'Set as Latest'


class ShaderVersionAdmin(admin.ModelAdmin):
    list_display=('shader','created_at','ip_addr','content')
    list_filter = ('shader',)
    search_fields = ('shader__title', 'content', 'ip_addr')
    actions = ['set_as_latest_version']

    def set_as_latest_version(self, request, queryset):
        # select one at a time
        if queryset.count() != 1:
            self.message_user(request, "Please select a single version to set as latest.", level='error')
            return
        
        clone_as_latest(self, request, queryset.first())

    set_as_latest_version.short_description = "Set selected version as latest"


class ShaderAdmin(admin.ModelAdmin):
    """
    Admin interface for Shader.
    """
    inlines = [ShaderVersionInline]
    list_display = ('title', 'author', 'ident', 'is_playable', 'is_playing', 'is_deleted', 'created_at')
    search_fields = ('title', 'author', 'ident')
    list_filter = ('is_playable', 'is_deleted', 'created_at', 'is_playing')

    def get_urls(self):
        """
        Add custom URLs for ShaderAdmin.
        """
        urls = super().get_urls()
        custom_urls = [
            path(
                '<int:shader_id>/set_latest/<int:version_id>/',
                self.admin_site.admin_view(self.set_latest_version),
                name='shader_app_shader_set_as_latest_version'
            ),
        ]
        return custom_urls + urls

    def set_latest_version(self, request, shader_id, version_id):
        """
        Custom admin view to set a specific ShaderVersion as the latest.
        """
        shader = get_object_or_404(Shader, pk=shader_id)
        version = get_object_or_404(ShaderVersion, pk=version_id, shader=shader)

        # Permission check
        if not request.user.has_perm('shader_app.change_shaderversion'):
            self.message_user(request, "You do not have permission to perform this action.", level=messages.ERROR)
            return redirect('admin:shader_app_shader_change', shader_id)

        clone_as_latest(self, request, version)

        # Redirect back to the Shader admin change page
        return redirect('admin:shader_app_shader_change', shader_id)


class PlaybackControlAdmin(admin.ModelAdmin):
    # the playlist fields are only written by the runner process
    fields = ('play_now_cooldown', 'play_now_cap', 'play_now_cap_minutes', 'auto_ban_rejections', 'auto_ban_hours')

    def has_add_permission(self, request):
        return False

    def has_delete_permission(self, request, obj=None):
        return False


class PlayNowRequestAdmin(admin.ModelAdmin):
    list_display = ('created_at', 'ip_link', 'shader', 'accepted', 'ip_rejected', 'ip_banned')
    list_filter = ('accepted', 'created_at')
    search_fields = ('ip_addr',)
    ordering = ['-created_at']
    actions = ['ban_ips']

    def get_queryset(self, request):
        rejected = PlayNowRequest.objects.filter(ip_addr=OuterRef('ip_addr'), accepted=False).values('ip_addr').annotate(n=Count('id')).values('n')
        return super().get_queryset(request).annotate(
            ip_rejected=Coalesce(Subquery(rejected), 0),
            ip_banned=Exists(BannedIP.objects.filter(ip_address=OuterRef('ip_addr'))),
        )

    @admin.display(description="IP address", ordering='ip_addr')
    def ip_link(self, obj):
        if obj.ip_addr is None:
            return "-"
        return format_html('<a href="?ip_addr={}">{}</a>', obj.ip_addr, obj.ip_addr)

    @admin.display(description="Rejected from this IP", ordering='ip_rejected')
    def ip_rejected(self, obj):
        return obj.ip_rejected

    @admin.display(description="IP banned", boolean=True, ordering='ip_banned')
    def ip_banned(self, obj):
        return obj.ip_banned

    @admin.action(description="Ban IP addresses of selected requests")
    def ban_ips(self, request, queryset):
        ips = set(queryset.exclude(ip_addr=None).values_list('ip_addr', flat=True))
        for ip in ips:
            BannedIP.objects.get_or_create(ip_address=ip, defaults={'reason': "Play Now spam"})
        self.message_user(request, f"Banned {len(ips)} IP address(es).", level=messages.SUCCESS)


admin.site.register(Shader, ShaderAdmin)
admin.site.register(ShaderVersion, ShaderVersionAdmin)
admin.site.register(ShaderPrefix)
admin.site.register(DefaultShaderEditorText)
admin.site.register(LedCoordsCfg)
admin.site.register(BannedIP)
admin.site.register(PlaybackControl, PlaybackControlAdmin)
admin.site.register(PlayNowRequest, PlayNowRequestAdmin)
