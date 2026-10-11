import json
from django.contrib.auth.models import User
from django.test import TestCase
from .models import Shader, ShaderVersion, ShaderPrefix, LedCoordsCfg, PlaybackControl, BannedIP


def make_shader(**kwargs):
    fields = {"title": "t", "author": "a", "is_playable": True}
    fields.update(kwargs)
    shader = Shader.objects.create(**fields)
    ShaderVersion.objects.create(shader=shader, content="void main() {}")
    return shader


class SaveApiTests(TestCase):
    def save(self, **kwargs):
        body = {"shaderid": "", "title": "t", "author": "a", "playable": True, "shadertext": "void main() {}"}
        body.update(kwargs)
        return self.client.post("/api/save", json.dumps(body), content_type="application/json", REMOTE_ADDR="10.0.0.1")

    def test_new(self):
        resp = self.save()
        self.assertEqual(resp.status_code, 201)
        shader = Shader.objects.get()
        self.assertEqual(resp.json()["redirect_url"], f"/edit/{shader.ident}/")
        self.assertTrue(shader.is_playable)
        version = shader.versions.get()
        self.assertEqual(version.content, "void main() {}")
        self.assertEqual(version.ip_addr, "10.0.0.1")

    def test_existing(self):
        shader = make_shader()
        resp = self.save(shaderid=shader.ident, title="t2", playable=False, shadertext="// v2")
        self.assertEqual(resp.status_code, 200)
        shader.refresh_from_db()
        self.assertEqual(shader.title, "t2")
        self.assertFalse(shader.is_playable)
        self.assertEqual(shader.versions.count(), 2)
        self.assertEqual(shader.versions.order_by("-created_at").first().content, "// v2")

    def test_missing_fields(self):
        for field in ("title", "author", "shadertext"):
            self.assertEqual(self.save(**{field: "  "}).status_code, 400)
        self.assertFalse(Shader.objects.exists())

    def test_unknown(self):
        self.assertEqual(self.save(shaderid="nope").status_code, 404)

    def test_get_rejected(self):
        self.assertEqual(self.client.get("/api/save").status_code, 405)

    def test_banned(self):
        BannedIP.objects.create(ip_address="10.0.0.1")
        self.assertEqual(self.save().status_code, 403)
        self.assertFalse(Shader.objects.exists())


class DeleteApiTests(TestCase):
    def delete(self, ident):
        return self.client.delete("/api/delete", json.dumps({"shaderid": ident}), content_type="application/json")

    def test_delete(self):
        shader = make_shader()
        resp = self.delete(shader.ident)
        self.assertEqual(resp.status_code, 200)
        shader.refresh_from_db()
        self.assertTrue(shader.is_deleted)
        self.assertEqual(shader.versions.order_by("-created_at").first().content, "// Shader deleted")
        self.assertEqual(self.delete(shader.ident).status_code, 404)

    def test_bad_requests(self):
        self.assertEqual(self.delete("").status_code, 400)
        self.assertEqual(self.delete("nope").status_code, 404)


class PlayNowApiTests(TestCase):
    def test_play_now(self):
        shader = make_shader()
        for nonce in (1, 2):
            resp = self.client.post(f"/api/play_now/{shader.ident}/")
            self.assertEqual(resp.status_code, 200)
            ctrl = PlaybackControl.objects.get(pk=1)
            self.assertEqual(ctrl.force_shader_id, shader.id)
            self.assertEqual(ctrl.force_nonce, nonce)

    def test_not_playable(self):
        for shader in (make_shader(is_playable=False), make_shader(is_deleted=True)):
            self.assertEqual(self.client.post(f"/api/play_now/{shader.ident}/").status_code, 404)
        self.assertEqual(self.client.post("/api/play_now/nope/").status_code, 404)
        self.assertFalse(PlaybackControl.objects.filter(force_shader__isnull=False).exists())

    def test_get_rejected(self):
        shader = make_shader()
        self.assertEqual(self.client.get(f"/api/play_now/{shader.ident}/").status_code, 405)


class EditorPageTests(TestCase):
    def setUp(self):
        LedCoordsCfg.objects.create(content=[0.0, 1.0, 2.0])

    def test_new(self):
        resp = self.client.get("/edit/new/")
        self.assertContains(resp, 'data-playseconds="30"')
        self.assertContains(resp, '<script id="ledcoords" type="application/json">[0.0, 1.0, 2.0]</script>', html=True)

    def test_prefix(self):
        ShaderPrefix.objects.create(content="#version 300 es")
        self.assertEqual(ShaderPrefix.program_prefix(), "#version 300 es\n")
        self.assertContains(self.client.get("/edit/new/"), 'data-shaderprefix="#version 300 es\n"')

    def test_existing(self):
        shader = make_shader(title="My shader")
        resp = self.client.get(f"/edit/{shader.ident}/")
        self.assertContains(resp, "My shader")
        self.assertContains(resp, "void main() {}")

    def test_deleted(self):
        shader = make_shader(is_deleted=True)
        self.assertEqual(self.client.get(f"/edit/{shader.ident}/").status_code, 404)
        self.assertEqual(self.client.get("/edit/nope/").status_code, 404)


class AdminSetLatestTests(TestCase):
    def test_view_and_action(self):
        self.client.force_login(User.objects.create_superuser("admin", "", "pw"))
        shader = make_shader()
        old = shader.versions.get()
        ShaderVersion.objects.create(shader=shader, content="// newer")

        resp = self.client.get(f"/admin/shader_app/shader/{shader.id}/set_latest/{old.id}/", REMOTE_ADDR="10.0.0.2")
        self.assertRedirects(resp, f"/admin/shader_app/shader/{shader.id}/change/")
        latest = shader.versions.order_by("-created_at").first()
        self.assertEqual((latest.content, latest.ip_addr), (old.content, "10.0.0.2"))

        self.client.post("/admin/shader_app/shaderversion/", {"action": "set_as_latest_version", "_selected_action": [old.id]})
        self.assertEqual(shader.versions.count(), 4)
