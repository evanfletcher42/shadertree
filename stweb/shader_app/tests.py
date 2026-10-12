import json
from datetime import datetime, timezone
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

    def test_keeps_runner_fields(self):
        shader = make_shader(is_playing=True, compile_failures=2, compile_error="bad")
        self.assertEqual(self.save(shaderid=shader.ident).status_code, 200)
        shader.refresh_from_db()
        self.assertTrue(shader.is_playing)
        self.assertEqual((shader.compile_failures, shader.compile_error), (0, ""))

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
        PlaybackControl.objects.create(pk=1, queue=[shader.id])
        resp = self.client.post(f"/api/play_now/{shader.ident}/")
        self.assertEqual(resp.status_code, 200)
        ctrl = PlaybackControl.objects.get(pk=1)
        self.assertEqual(ctrl.force_shader_id, shader.id)
        self.assertEqual(ctrl.queue, [shader.id])

    def test_not_playable(self):
        for shader in (make_shader(is_playable=False), make_shader(is_deleted=True)):
            self.assertEqual(self.client.post(f"/api/play_now/{shader.ident}/").status_code, 404)
        self.assertEqual(self.client.post("/api/play_now/nope/").status_code, 404)
        self.assertFalse(PlaybackControl.objects.filter(force_shader__isnull=False).exists())

    def test_get_rejected(self):
        shader = make_shader()
        self.assertEqual(self.client.get(f"/api/play_now/{shader.ident}/").status_code, 405)

    def test_index_buttons(self):
        on, off = make_shader(), make_shader(is_playable=False)
        resp = self.client.get("/")
        self.assertIn("csrftoken", resp.cookies)
        self.assertContains(resp, f'<tr class="tableRow" data-ident="{on.ident}">')
        self.assertContains(resp, '<td class="tableCell playNow" data-playable="1"></td>', count=1, html=True)
        self.assertContains(resp, '<td class="tableCell playNow" data-playable=""></td>', count=1, html=True)


class EditorPageTests(TestCase):
    def setUp(self):
        LedCoordsCfg.objects.create(content=[0.0, 1.0, 2.0])

    def test_new(self):
        resp = self.client.get("/edit/new/")
        self.assertContains(resp, 'data-playseconds="30"')
        self.assertContains(resp, 'data-ledgamma="1.7"')
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

    def test_next_play(self):
        shader = make_shader()
        self.assertNotContains(self.client.get(f"/edit/{shader.ident}/"), "Plays next")
        PlaybackControl.objects.filter(pk=1).update(queue=[shader.id], started_at=datetime(2026, 12, 1, 20, 0, tzinfo=timezone.utc))
        self.assertContains(self.client.get(f"/edit/{shader.ident}/"), "Plays next on the tree at 12:00:30 PM.")
        Shader.objects.filter(pk=shader.pk).update(is_playing=True)
        self.assertContains(self.client.get(f"/edit/{shader.ident}/"), "Playing on the tree now.")

    def test_compile_error(self):
        shader = make_shader(compile_failures=1, compile_error="ERROR: 0:3: oops")
        resp = self.client.get(f"/edit/{shader.ident}/")
        self.assertContains(resp, "ERROR: 0:3: oops")
        self.assertNotContains(resp, "taken off the tree")
        Shader.objects.filter(pk=shader.pk).update(compile_failures=3, is_playable=False)
        self.assertContains(self.client.get(f"/edit/{shader.ident}/"), "taken off the tree")

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


class DocsFromDataTests(TestCase):
    def test_led_count(self):
        LedCoordsCfg.objects.create(content=[0, -0.48, 0, 0.5, 0, 0, 0, 0, -1, 0, -0.48, 0])
        self.assertEqual(LedCoordsCfg.objects.get().led_count(), 2)
        self.assertContains(self.client.get("/about"), "made out of 2 addressable LEDs")

    def test_index_newest_first(self):
        old, new = make_shader(title="Old shader"), make_shader(title="New shader")
        content = self.client.get("/").content.decode()
        self.assertLess(content.index("New shader"), content.index("Old shader"))

    def test_index_period(self):
        self.assertContains(self.client.get("/"), "30 seconds each")


class PlaylistTests(TestCase):
    def test_cycle(self):
        ctrl = PlaybackControl.get()
        ids = [1, 2, 3, 4]
        ctrl.sync(ids)
        for cycle in range(20):
            self.assertEqual(sorted(ctrl.next_cycle), ids)
            planned = ctrl.queue + ctrl.next_cycle if cycle else None
            order = [ctrl.take_next() for _ in ids]
            self.assertEqual(sorted(order), ids)
            if cycle:
                self.assertEqual(order, planned[:4])
                self.assertNotEqual(order[0], last)
            last = order[-1]

    def test_nothing_playable(self):
        ctrl = PlaybackControl.get()
        ctrl.sync([])
        self.assertIsNone(ctrl.take_next())

    def test_sync(self):
        ctrl = PlaybackControl(queue=[3, 4], next_cycle=[2, 4, 1, 3], current=2)
        for _ in range(20):
            ctrl.sync([1, 2, 3, 4, 5])
            self.assertEqual(sorted(ctrl.queue), [3, 4, 5])
            self.assertEqual([i for i in ctrl.queue if i != 5], [3, 4])
            self.assertEqual([i for i in ctrl.next_cycle if i != 5], [2, 4, 1, 3])
            ctrl.queue.remove(5)
            ctrl.next_cycle.remove(5)
        ctrl.sync([1, 3])
        self.assertEqual((ctrl.queue, ctrl.next_cycle), ([3], [1, 3]))

    def test_avoid_repeat(self):
        ctrl = PlaybackControl(queue=[3, 4], next_cycle=[1, 4, 2, 3])
        ctrl.sync([2, 3, 4])
        self.assertEqual(ctrl.next_cycle, [2, 4, 3])
        ctrl.queue = []
        ctrl.current = 2
        ctrl.sync([2, 3, 4])
        self.assertEqual(ctrl.next_cycle, [4, 2, 3])

    def test_play_next(self):
        ctrl = PlaybackControl(queue=[3, 4, 5], next_cycle=[1, 2, 3, 4, 5], current=2)
        ctrl.play_next(5)
        self.assertEqual(ctrl.queue, [5, 3, 4])
        ctrl.play_next(1)
        self.assertEqual(ctrl.queue, [1, 5, 3, 4])
        self.assertEqual(ctrl.take_next(), 1)

    def test_upcoming(self):
        a, b = make_shader(), make_shader()
        start = datetime(2026, 12, 1, 20, 0, tzinfo=timezone.utc)
        ctrl = PlaybackControl(queue=[b.id], next_cycle=[a.id, 999, b.id], started_at=start)
        self.assertEqual([(s, eta.minute, eta.second) for s, eta in ctrl.upcoming()], [(b, 0, 30), (a, 1, 0), (b, 2, 0)])
        ctrl.started_at = None
        self.assertEqual(ctrl.upcoming(), [(b, None), (a, None), (b, None)])

    def test_api(self):
        a, b = make_shader(title="A", is_playing=True), make_shader(title="B")
        PlaybackControl.objects.create(pk=1, queue=[b.id], next_cycle=[b.id, a.id], current=a.id,
                                       started_at=datetime(2026, 12, 1, 20, 0, tzinfo=timezone.utc))
        self.assertEqual(self.client.get("/api/playlist").json(), {
            "now_playing": {"ident": a.ident, "title": "A", "author": "a"},
            "upcoming": [{"ident": b.ident, "title": "B", "author": "a", "eta": "12:00:30 PM"},
                         {"ident": b.ident, "title": "B", "author": "a", "eta": "12:01:00 PM"},
                         {"ident": a.ident, "title": "A", "author": "a", "eta": "12:01:30 PM"}]
        })
        resp = self.client.get("/")
        self.assertContains(resp, "<td class=\"tableCell nextPlay\"> Now </td>", html=True)
        self.assertContains(resp, "<td class=\"tableCell nextPlay\"> 12:00:30 PM </td>", html=True)

        # after A's slot it still shows its next play, in the next cycle
        Shader.objects.filter(pk=a.pk).update(is_playing=False)
        self.assertContains(self.client.get("/"), "<td class=\"tableCell nextPlay\"> 12:01:30 PM </td>", html=True)


class RunnerTests(TestCase):
    def setUp(self):
        import runner_process
        self.runner = runner_process

    def test_record_compile(self):
        shader = make_shader()
        for _ in range(3):
            self.runner.record_compile(shader.id, "oops")
        shader.refresh_from_db()
        self.assertEqual((shader.compile_failures, shader.compile_error, shader.is_playable), (3, "oops", False))
        Shader.objects.filter(pk=shader.pk).update(is_playable=True)
        self.runner.record_compile(shader.id, None)
        shader.refresh_from_db()
        self.assertEqual((shader.compile_failures, shader.compile_error, shader.is_playable), (0, "", True))

    def test_refresh_play_now(self):
        a, b, off = make_shader(), make_shader(), make_shader(is_playable=False)
        ctrl = PlaybackControl.objects.create(pk=1, queue=[a.id, b.id], next_cycle=[a.id, b.id])
        self.assertFalse(self.runner.refresh(ctrl))
        PlaybackControl.objects.filter(pk=1).update(force_shader=b)
        self.assertTrue(self.runner.refresh(ctrl))
        self.assertEqual(PlaybackControl.objects.get(pk=1).queue, [b.id, a.id])
        self.assertIsNone(PlaybackControl.objects.get(pk=1).force_shader_id)
        PlaybackControl.objects.filter(pk=1).update(force_shader=off)
        self.assertFalse(self.runner.refresh(ctrl))
        self.assertIsNone(PlaybackControl.objects.get(pk=1).force_shader_id)
