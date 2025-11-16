"""
Generic utilities for shader_app
"""

def get_client_ip(request):
    """Retrieve the client's IP address from the request."""
    x_forwarded_for = request.META.get('HTTP_X_FORWARDED_FOR')
    if x_forwarded_for:
        # X-Forwarded-For may contain multiple IPs; the first one is the client's IP
        ip = x_forwarded_for.split(',')[0].strip()
    else:
        ip = request.META.get('REMOTE_ADDR')
    return ip

