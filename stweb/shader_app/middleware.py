from django.http import HttpResponseForbidden
from django.shortcuts import render
from .utils import get_client_ip
from .models import BannedIP

class IPBanMiddleware:
    def __init__(self, get_response):
        self.get_response = get_response

    def __call__(self, request):
        client_ip = get_client_ip(request)
        if BannedIP.objects.filter(ip_address=client_ip).exists():
            return render(request, 'shader_app/403_banned.html', status=403)
        return self.get_response(request)
