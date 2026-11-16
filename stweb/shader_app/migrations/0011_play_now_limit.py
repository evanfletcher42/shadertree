import django.db.models.deletion
from django.db import migrations, models


class Migration(migrations.Migration):

    dependencies = [
        ('shader_app', '0010_next_cycle'),
    ]

    operations = [
        migrations.AddField(
            model_name='playbackcontrol',
            name='play_now_cooldown',
            field=models.PositiveIntegerField(default=60, help_text='Seconds an IP address must wait between Play Now requests.'),
        ),
        migrations.AddField(
            model_name='playbackcontrol',
            name='play_now_cap',
            field=models.PositiveIntegerField(default=10, help_text='Accepted Play Now requests allowed per IP address in the cap window. 0 turns the cap off.'),
        ),
        migrations.AddField(
            model_name='playbackcontrol',
            name='play_now_cap_minutes',
            field=models.PositiveIntegerField(default=60, help_text='Length of the cap window, in minutes.'),
        ),
        migrations.AddField(
            model_name='playbackcontrol',
            name='auto_ban_rejections',
            field=models.PositiveIntegerField(default=20, help_text='Ban an IP address with more than this many rejected Play Now requests in the ban window. 0 turns auto-ban off.'),
        ),
        migrations.AddField(
            model_name='playbackcontrol',
            name='auto_ban_hours',
            field=models.PositiveIntegerField(default=24, help_text='Length of the ban window, in hours.'),
        ),
        migrations.CreateModel(
            name='PlayNowRequest',
            fields=[
                ('id', models.BigAutoField(auto_created=True, primary_key=True, serialize=False, verbose_name='ID')),
                ('created_at', models.DateTimeField(auto_now_add=True)),
                ('ip_addr', models.GenericIPAddressField(blank=True, null=True)),
                ('accepted', models.BooleanField()),
                ('shader', models.ForeignKey(null=True, on_delete=django.db.models.deletion.SET_NULL, to='shader_app.shader')),
            ],
        ),
    ]
