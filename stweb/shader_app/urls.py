from django.urls import path

from . import views

urlpatterns = [
    path("", views.index, name="index"),
    path("shader_list/", views.shader_list, name="shader_list"),
    path("edit/new/", views.edit_new, name="edit_new"),
    path("edit/<str:shader_ident>/", views.edit_existing, name="edit_existing"),
    path("now-playing", views.now_playing, name="now_playing"),
    path("about", views.about, name="about"),
    path("api/save", views.api_edit_shader, name="api_edit_shader"),
    path("api/delete", views.api_delete_shader, name="api_delete_shader"),
    path("api/now_playing", views.api_now_playing, name="api_now_playing"),
    path("api/play_now/<str:ident>/", views.play_now, name="api_play_now"),]
