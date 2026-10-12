from django.shortcuts import render, get_object_or_404
from django.http import HttpResponse, JsonResponse, Http404
from django.template import loader
from django.views.decorators.http import require_GET, require_POST, require_http_methods
from django.views.decorators.csrf import ensure_csrf_cookie
from django.conf import settings
from django.utils import timezone
from django.utils.dateformat import time_format
from .utils import get_client_ip
from .models import Shader, ShaderVersion, ShaderPrefix, DefaultShaderEditorText, LedCoordsCfg, PlaybackControl
import json

def eta_text(eta):
    return "~" + time_format(timezone.localtime(eta), "g:i:s A") if eta else ""

def next_plays():
    """Shader id -> when it next plays on the tree, as display text."""
    texts = {}
    for shader, eta in PlaybackControl.get().upcoming():
        texts.setdefault(shader.id, eta_text(eta))
    for shader_id in Shader.objects.filter(is_playing=True).values_list("id", flat=True):
        texts[shader_id] = "Now"
    return texts

def shader_info(shader):
    return {"ident": shader.ident, "title": shader.title, "author": shader.author}

@ensure_csrf_cookie
def index(request):
    template = loader.get_template("shader_app/index.html")

    shaders = Shader.objects.filter(is_deleted=False).order_by('-created_at').values('id', 'title', 'author', 'created_at', 'ident', 'is_playable')
    shader_list = list(shaders)  # QuerySet->List
    plays = next_plays()
    for s in shader_list:
        s["next_play"] = plays.get(s["id"], "")

    context = {
        "shaders": shader_list,
        "playseconds": settings.SHADER_PLAY_SECONDS
    }
    return HttpResponse(template.render(context, request))

@ensure_csrf_cookie
def now_playing(request):
    template = loader.get_template("shader_app/now-playing.html")
    context = {}

    return HttpResponse(template.render(context, request))

def render_editor(request, context):
    coords_obj = LedCoordsCfg.objects.first()
    context.update({
        "ledcoords": coords_obj.content if coords_obj else [],
        "shaderprefix": ShaderPrefix.program_prefix(),
        "playseconds": settings.SHADER_PLAY_SECONDS,
        "ledgamma": settings.LED_GAMMA
    })
    return render(request, "shader_app/editor.html", context)


def edit_new(request):
    editor_obj = DefaultShaderEditorText.objects.first()
    return render_editor(request, {
        "shaderident": "",
        "shadertitle": "",
        "shaderauthor": "",
        "shaderplayable": True,
        "editortext": editor_obj.content if editor_obj else ""
    })


def editor_next_play(shader):
    text = next_plays().get(shader.id, "")
    if text == "Now":
        return "Playing on the tree now."
    return f"Plays next on the tree at {text}." if text else ""


def edit_existing(request, shader_ident):
    shader = get_object_or_404(Shader, ident=shader_ident, is_deleted=False)
    latest_shader_version = shader.versions.order_by('-created_at').first()
    return render_editor(request, {
        "shaderident": shader.ident,
        "shadertitle": shader.title,
        "shaderauthor": shader.author,
        "shaderplayable": shader.is_playable,
        "editortext": latest_shader_version.content if latest_shader_version else "",
        "nextplay": editor_next_play(shader),
        "compileerror": shader.compile_error,
        "takenoff": not shader.is_playable and shader.compile_failures >= settings.SHADER_MAX_COMPILE_FAILURES,
    })


def about(request):
    coords_obj = LedCoordsCfg.objects.first()
    return render(request, "shader_app/about.html", {
        "ledcount": coords_obj.led_count() if coords_obj else 0,
        "playseconds": settings.SHADER_PLAY_SECONDS
    })


@require_POST
def api_edit_shader(request):
    data = json.loads(request.body)

    shader_ident = data.get('shaderid', '').strip()
    shader_title = data.get('title', '').strip()
    shader_author = data.get('author', '').strip()
    shader_playable = data.get('playable', False)
    shader_text = data.get('shadertext', '').strip()

    if not shader_text:
        return JsonResponse({'error': 'shadertext cannot be empty.'}, status=400)

    if not shader_title:
        return JsonResponse({'error': 'title cannot be empty.'}, status=400)

    if not shader_author:
        return JsonResponse({'error': 'author cannot be empty.'}, status=400)
    
    if not shader_ident:
        # New shader
        shader = Shader.objects.create(
            author=shader_author,
            title=shader_title,
            is_playable=shader_playable
        )

        ShaderVersion.objects.create(
            shader=shader,
            ip_addr=get_client_ip(request),
            content=shader_text
        )

        # redirect to edit page for this new shader
        return JsonResponse({
            'redirect_url': f'/edit/{shader.ident}/',
            'message': 'New shader created successfully'
            }, status=201)
    else:
        # edit existing shader
        try:
            shader = get_object_or_404(Shader, ident=shader_ident)
        except Http404:
            return JsonResponse({
                'message': 'Requested shader not found'
                }, status=404)

        # update() so a concurrent runner write to is_playing isn't clobbered
        # a new version gets a fresh set of compile attempts on the tree
        Shader.objects.filter(pk=shader.pk).update(
            title=shader_title,
            author=shader_author,
            is_playable=shader_playable,
            compile_failures=0,
            compile_error=""
        )

        # new ShaderVersion
        ShaderVersion.objects.create(
            shader=shader,
            ip_addr=get_client_ip(request),
            content=shader_text
        )

        return JsonResponse({
            'message': 'Shader modified successfully'
            }, status=200)

@require_http_methods("DELETE")
def api_delete_shader(request):
    data = json.loads(request.body)
    shader_ident = data.get('shaderid', '').strip()

    if not shader_ident:
        return JsonResponse({
            'message': 'No shader specified'
            }, status=400)

    try:
        shader = get_object_or_404(Shader, ident=shader_ident)
    except Http404:
        return JsonResponse({
            'message': 'Requested shader not found'
            }, status=404)

    if shader.is_deleted:
        return JsonResponse({
            'message': 'Requested shader not found a'
            }, status=404)
    
    # Make a ShaderVersion for logging purposes
    ShaderVersion.objects.create(
        shader=shader,
        ip_addr=get_client_ip(request),
        content="// Shader deleted"
    )

    Shader.objects.filter(pk=shader.pk).update(is_deleted=True)

    return JsonResponse({
            'message': 'Shader deleted successfully',
            'redirect_url': "/"
            }, status=200)


@require_GET
def shader_list(request):
    shaders = Shader.objects.filter(is_deleted=False).order_by('created_at').values('title', 'author', 'ident', 'is_playable')
    shader_list = list(shaders)  # QuerySet->List
    return JsonResponse(shader_list, safe=False)


@require_GET
def api_now_playing(request):
    try:
        shader = Shader.objects.get(is_playing=True)
        data = {
            "now_playing": shader.title,
            "author": shader.author
        }
    except Shader.DoesNotExist:
        data = {
            "now_playing": None,
            "author": None
        }
    return JsonResponse(data)

@require_GET
def api_playlist(request):
    playing = Shader.objects.filter(is_playing=True).first()
    return JsonResponse({
        "now_playing": shader_info(playing) if playing else None,
        "upcoming": [dict(shader_info(s), eta=eta_text(eta)) for s, eta in PlaybackControl.get().upcoming()]
    })

@require_POST
def play_now(request, ident: str):
    shader = get_object_or_404(Shader, ident=ident, is_deleted=False, is_playable=True)

    # update() only touches the request; the runner owns the playlist fields
    PlaybackControl.get()
    PlaybackControl.objects.filter(pk=1).update(force_shader_id=shader.id)

    return JsonResponse({"ok": True, "ident": shader.ident, "title": shader.title})
