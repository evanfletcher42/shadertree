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

// Poll /api/playlist every PLAYLIST_POLL_MS and pass results to onUpdate
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

let playNowBlockedUntil = 0;

function setPlayNowState(button) {
    const wait = Math.ceil((playNowBlockedUntil - Date.now()) / 1000);
    button.disabled = wait > 0;
    button.textContent = wait > 0 ? `Wait ${Math.floor(wait / 60)}:${String(wait % 60).padStart(2, '0')}` : 'Play Now';
}

function updatePlayNowButtons() {
    document.querySelectorAll('.playNowButton, #playNowBtn').forEach(setPlayNowState);
    if (Date.now() < playNowBlockedUntil)
        setTimeout(updatePlayNowButtons, 1000);
}

// Ask the tree to play this shader next
async function playNow(ident) {
    try {
        const response = await fetch(`/api/play_now/${encodeURIComponent(ident)}/`, {
            method: 'POST',
            credentials: 'same-origin',
            headers: { 'X-CSRFToken': getCookie('csrftoken') },
        });
        if (!response.ok && response.status !== 429) throw new Error(`Response status: (${response.status}) ${await response.text()}`);
        playNowBlockedUntil = Date.now() + (await response.json()).retry_after * 1000;
        updatePlayNowButtons();
    } catch (error) {
        console.error('Play-now failed:', error);
    }
}

function makePlayNowButton(ident) {
    const button = document.createElement('button');
    button.className = 'playNowButton';
    setPlayNowState(button);
    button.addEventListener('click', () => playNow(ident));
    return button;
}
