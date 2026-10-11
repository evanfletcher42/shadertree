from django.shortcuts import render, get_object_or_404
from django.http import HttpResponse, JsonResponse, Http404
from django.template import loader
from django.views.decorators.http import require_GET, require_POST, require_http_methods
from django.views.decorators.csrf import ensure_csrf_cookie
from django.db import transaction
from django.db.models import F
from .utils import get_client_ip
from .models import Shader, ShaderVersion, ShaderPrefix, DefaultShaderEditorText, LedCoordsCfg, PlaybackControl
import json

def index(request):
    template = loader.get_template("shader_app/index.html")

    shaders = Shader.objects.filter(is_deleted=False).order_by('created_at').values('title', 'author', 'created_at', 'ident', 'is_playable')
    shader_list = list(shaders)  # QuerySet->List

    context = {
        "shaders": shader_list
    }
    return HttpResponse(template.render(context, request))

@ensure_csrf_cookie
def now_playing(request):
    template = loader.get_template("shader_app/now-playing.html")
    context = {}

    return HttpResponse(template.render(context, request))

def edit_new(request):
    template = loader.get_template("shader_app/editor.html")

    # load led positions
    coords_list = []  # TBD: Default for debug?
    coords_obj = LedCoordsCfg.objects.first()
    if coords_obj:
        coords_list = coords_obj.content

    coords_str = json.dumps(coords_list)

    # load shader prefix
    try:
        pfx_obj = ShaderPrefix.objects.first()
        prefix_text = pfx_obj.content if pfx_obj else ""
    except ShaderPrefix.DoesNotExist:
        prefix_text = ""

    # Load default shader content
    try:
        editor_obj = DefaultShaderEditorText.objects.first()
        editor_text = editor_obj.content if editor_obj else ""
    except DefaultShaderEditorText.DoesNotExist:
        editor_text = ""

    context = {
        "shaderident": "",
        "shadertitle": "",
        "shaderauthor": "",
        "shaderplayable": True,
        "ledcoords": coords_str,
        "shaderprefix": prefix_text,
        "editortext": editor_text
    } 
    
    return HttpResponse(template.render(context, request))


def edit_existing(request, shader_ident):
    try:
        shader = get_object_or_404(Shader, ident=shader_ident)
    except Http404:
        raise Http404("Page not found.")

    if shader.is_deleted:
        raise Http404("Page not found.")

    template = loader.get_template("shader_app/editor.html")

    # load led positions
    coords_list = []  # TBD: Default for debug?
    coords_obj = LedCoordsCfg.objects.first()
    if coords_obj:
        coords_list = coords_obj.content

    coords_str = json.dumps(coords_list)

    # load shader prefix
    try:
        pfx_obj = ShaderPrefix.objects.first()
        prefix_text = pfx_obj.content if pfx_obj else ""
    except ShaderPrefix.DoesNotExist:
        prefix_text = ""

    # load shader content
    try:
        latest_shader_version = shader.versions.order_by('-created_at').first()
        editor_text = latest_shader_version.content if latest_shader_version else ""
    except ShaderVersion.DoesNotExist:
        editor_text = ""

    context = {
        "shaderident": shader.ident,
        "shadertitle": shader.title,
        "shaderauthor": shader.author,
        "shaderplayable": shader.is_playable,
        "ledcoords": coords_str,
        "shaderprefix": prefix_text,
        "editortext": editor_text
    }

    return HttpResponse(template.render(context, request))


def about(request):
    template = loader.get_template("shader_app/about.html")
    context = {}
    return HttpResponse(template.render(context, request))


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

        # set fields
        shader.title = shader_title
        shader.author = shader_author
        shader.is_playable = shader_playable
        shader.save()

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

    shader.is_deleted = True
    shader.save()

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
def led_positions(request):
    coords_obj = LedCoordsCfg.objects.first()
    if not coords_obj:
        return JsonResponse({'error': 'LED configuration missing on server.'}, status=500)

    return JsonResponse({"coords": coords_obj.content})

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

@require_POST
def play_now(request, ident: str):
    shader = get_object_or_404(Shader, ident=ident, is_deleted=False, is_playable=True)

    # ensure singleton exists.
    PlaybackControl.objects.get_or_create(pk=1)

    with transaction.atomic():
        PlaybackControl.objects.filter(pk=1).update(
            force_shader_id=shader.id,          # <-- IMPORTANT
            force_nonce=F("force_nonce") + 1,
        )

    return JsonResponse({"ok": True, "ident": shader.ident, "title": shader.title})
