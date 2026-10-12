// Shared by the index and now-playing pages

const PLAYLIST_POLL_MS = 2000;

function getCookie(name) {
    // Standard Django CSRF cookie helper
    let cookieValue = null;
    if (document.cookie && document.cookie !== '') {
        const cookies = document.cookie.split(';');
        for (let i = 0; i < cookies.length; i++) {
            const cookie = cookies[i].trim();
            if (cookie.substring(0, name.length + 1) === (name + '=')) {
                cookieValue = decodeURIComponent(cookie.substring(name.length + 1));
                break;
            }
        }
    }
    return cookieValue;
}

// Calls onUpdate with /api/playlist now and every PLAYLIST_POLL_MS
function pollPlaylist(onUpdate) {
    async function tick() {
        try {
            const response = await fetch('/api/playlist');
            if (!response.ok) throw new Error(`Response status: (${response.status})`);
            onUpdate(await response.json());
        } catch (error) {
            console.error('Failed to fetch playlist:', error);
        }
    }
    tick();
    setInterval(tick, PLAYLIST_POLL_MS);
}
