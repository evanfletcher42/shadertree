"""
Generic utilities for shader_app
"""

def get_client_ip(request):
    """Retrieve the client's IP address from the request."""
    # nginx sets X-Real-IP to the connecting address (deploy/nginx-shadertree.conf).
    # X-Forwarded-For isn't used: clients can put any address in it.
    return request.META.get('HTTP_X_REAL_IP') or request.META.get('REMOTE_ADDR')
