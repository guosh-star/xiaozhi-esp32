# Music proxy service for ESP32 cuckoo clock (async edition)
# Sources: NetEase Cloud Music (primary) + QQ Music (fallback)
# Start: python server_async.py  |  Listen: http://0.0.0.0:8765
# Requirements: pip install aiohttp

import asyncio
import json
import time
import os
import ssl
import sys
import subprocess
from collections import defaultdict

PORT = 8765

# Default UA
ESP_UA = "CuckooClock-ESP32/1.0 (Xiaozhi)"
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache")
os.makedirs(CACHE_DIR, exist_ok=True)

# --- Structured logging ---
def _log(tag, msg):
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{ts}] [{tag}] {msg}")

# Rate limiting (async-safe)
_ip_requests = defaultdict(list)
_RATE_LIMIT_WINDOW = 60
_RATE_LIMIT_MAX = 30

def _check_rate_limit(client_ip):
    now = time.time()
    window = _ip_requests[client_ip]
    window[:] = [t for t in window if now - t < _RATE_LIMIT_WINDOW]
    if len(window) >= _RATE_LIMIT_MAX:
        return False
    window.append(now)
    return True

# Concurrency limiter
_semaphore = asyncio.Semaphore(8)

# SSL context (skip cert verification for CDN servers)
def _ssl_ctx():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx

# ===========================================================================
# NetEase Cloud Music API (async)
# ===========================================================================

async def netease_search(keyword, limit=10):
    from urllib.parse import urlencode
    params = urlencode({"type": 1, "limit": limit, "s": keyword})
    url = f"https://music.163.com/api/search/get?{params}"
    headers = {"User-Agent": UA, "Referer": "https://music.163.com"}
    try:
        conn = aiohttp.TCPConnector(ssl=_ssl_ctx())
        async with aiohttp.ClientSession(connector=conn, timeout=aiohttp.ClientTimeout(total=10)) as sess:
            async with sess.get(url, headers=headers) as resp:
                data = await resp.json(content_type=None)
    except Exception as e:
        return None, f"NetEase search failed: {e}"

    if data is None:
        return [], None

    songs = []
    for item in data.get("result", {}).get("songs", []):
        songs.append({
            "id": item.get("id", 0),
            "mid": str(item.get("id", 0)),
            "name": item.get("name", ""),
            "singer": "/".join(a.get("name", "") for a in item.get("artists", [])),
            "album": item.get("album", {}).get("name", "Unknown"),
            "duration": item.get("duration", 0) // 1000,
            "source": "netease",
        })
    return songs, None


async def netease_play_url(song_id, br=320000):
    url = f"https://music.163.com/api/song/enhance/player/url?id={song_id}&ids=[{song_id}]&br={br}"
    headers = {"User-Agent": UA, "Referer": "https://music.163.com"}
    try:
        conn = aiohttp.TCPConnector(ssl=_ssl_ctx())
        async with aiohttp.ClientSession(connector=conn, timeout=aiohttp.ClientTimeout(total=10)) as sess:
            async with sess.get(url, headers=headers) as resp:
                data = await resp.json(content_type=None)
    except Exception as e:
        return None, f"NetEase get play URL failed: {e}"

    if data is None:
        return None, "Empty response"

    song_data = data.get("data", [{}])[0]
    play_url = song_data.get("url", "")
    if not play_url:
        return None, "No playable URL (VIP-only or removed)"
    return play_url, None


# ===========================================================================
# QQ Music API (async)
# ===========================================================================

QQ_SEARCH_URL = "https://c.y.qq.com/soso/fcgi-bin/client_search_cp"
QQ_SONG_URL = "https://u.y.qq.com/cgi-bin/musicu.fcg"


async def qq_search(keyword, limit=10):
    from urllib.parse import urlencode
    params = urlencode({
        "format": "json", "w": keyword, "n": limit,
        "p": 1, "cr": 1, "type": 0, "new_json": 1,
    })
    url = f"{QQ_SEARCH_URL}?{params}"
    headers = {"User-Agent": UA, "Referer": "https://y.qq.com"}
    try:
        conn = aiohttp.TCPConnector(ssl=_ssl_ctx())
        async with aiohttp.ClientSession(connector=conn, timeout=aiohttp.ClientTimeout(total=10)) as sess:
            async with sess.get(url, headers=headers) as resp:
                data = await resp.json(content_type=None)
    except Exception as e:
        return None, f"QQ search failed: {e}"

    if data is None:
        return [], None

    songs = []
    for item in data.get("data", {}).get("song", {}).get("list", []):
        songs.append({
            "id": item.get("mid", ""),
            "mid": item.get("mid", ""),
            "name": item.get("name", ""),
            "singer": "/".join(s.get("name", "") for s in item.get("singer", [])),
            "album": item.get("album", {}).get("name", "Unknown"),
            "duration": item.get("interval", 0),
            "source": "qq",
        })
    return songs, None


async def qq_play_url(song_mid):
    guid = str(int(time.time() * 1000)) + "0" * 8
    payload = {
        "req_0": {
            "module": "vkey.GetVkeyServer",
            "method": "CgiGetVkey",
            "param": {
                "guid": guid, "songmid": [song_mid], "songtype": [0],
                "uin": "0", "loginflag": 1, "platform": "20",
            }
        }
    }
    headers = {
        "User-Agent": UA, "Referer": "https://y.qq.com",
        "Content-Type": "application/json",
    }
    try:
        conn = aiohttp.TCPConnector(ssl=_ssl_ctx())
        async with aiohttp.ClientSession(connector=conn, timeout=aiohttp.ClientTimeout(total=10)) as sess:
            async with sess.post(QQ_SONG_URL, json=payload, headers=headers) as resp:
                data = await resp.json(content_type=None)
    except Exception as e:
        return None, f"QQ get play URL failed: {e}"

    if data is None:
        return None, "Empty response"

    result = data.get("req_0", {}).get("data", {})
    sip = result.get("sip", [])
    midurlinfo = result.get("midurlinfo", [])
    if not sip or not midurlinfo:
        return None, "No play info available"

    purl = midurlinfo[0].get("purl", "")
    if not purl:
        return None, "VIP-only or removed"
    server = sip[0]
    if not server.endswith("/"):
        server += "/"
    return server + purl, None


# ===========================================================================
# Unified search + play (async)
# ===========================================================================

_search_cache = {}
_SEARCH_CACHE_TTL = 300

async def unified_search(keyword, limit=10):
    cache_key = (keyword.lower().strip(), limit)
    now = time.time()
    if cache_key in _search_cache:
        songs, err, ts = _search_cache[cache_key]
        if now - ts < _SEARCH_CACHE_TTL:
            return songs, err

    all_songs = []
    songs, _ = await netease_search(keyword, limit)
    if songs:
        all_songs.extend(songs)
    songs2, _ = await qq_search(keyword, min(5, limit))
    if songs2:
        existing = {(s["name"], s["singer"]) for s in all_songs}
        for s in songs2:
            if (s["name"], s["singer"]) not in existing:
                all_songs.append(s)

    result = (all_songs[:limit], None) if all_songs else (None, "No results from either platform")
    _search_cache[cache_key] = (result[0], result[1], now)
    return result


async def get_play_url_for(song):
    source = song.get("source", "qq")
    if source == "netease":
        return await netease_play_url(int(song.get("id", 0)))
    return await qq_play_url(song.get("mid", song.get("id", "")))


async def _find_playable(keyword):
    songs, err = await unified_search(keyword, 5)
    if err or not songs:
        return None, None
    # Try in parallel — fastest playable URL wins
    tasks = [asyncio.create_task(get_play_url_for(s)) for s in songs[:3]]
    for i, task in enumerate(tasks):
        try:
            url, e = await task
            if url:
                return songs[i], url
        except:
            pass
    return None, None


# ===========================================================================
# Local cache (sync — fine for async handlers)
# ===========================================================================

def _cache_path(song_id):
    safe_id = str(song_id).replace("/", "_").replace("\\", "_")
    return os.path.join(CACHE_DIR, f"{safe_id}.mp3")

def _cache_get(song_id):
    path = _cache_path(song_id)
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    return None

def _cache_put(song_id, data):
    path = _cache_path(song_id)
    with open(path, "wb") as f:
        f.write(data)

def _cache_cleanup(max_mb=500, max_age_days=30):
    try:
        total = 0
        files = []
        for fname in os.listdir(CACHE_DIR):
            fpath = os.path.join(CACHE_DIR, fname)
            if os.path.isfile(fpath) and not fname.startswith('.'):
                sz = os.path.getsize(fpath)
                total += sz
                files.append((fpath, os.path.getmtime(fpath), sz))
        total_mb = total / (1024 * 1024)
        now = time.time()
        if total_mb > max_mb:
            files.sort(key=lambda x: x[1])
            for fpath, mtime, sz in files:
                if total_mb <= max_mb * 0.7:
                    break
                if now - mtime > max_age_days * 86400:
                    os.remove(fpath)
                    total -= sz
                    total_mb = total / (1024 * 1024)
                    _log("cache", f"Removed old: {os.path.basename(fpath)} ({total_mb:.1f}MB)")
    except Exception as e:
        _log("cache", f"Cleanup error: {e}")


# ===========================================================================
# aiohttp HTTP server
# ===========================================================================

# --- aiohttp Chinese URL patch ---
# ESP32 sends raw UTF-8 bytes in URL query strings (e.g. ?q=月亮代表我的心).
# aiohttp's C HTTP parser rejects these. Force pure Python parser instead.
import os as _os
_os.environ['AIOHTTP_NO_EXTENSIONS'] = '1'
# ------------------------------------

import aiohttp
from aiohttp import web

# HTML template (unescaped for readability)
HOME_HTML = r"""<!DOCTYPE html>
<html lang="zh">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Music Proxy</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui;max-width:800px;margin:2rem auto;padding:0 1rem;background:#111;color:#eee}
h1{color:#ec4141;margin-bottom:.5rem}
h1 span{color:#31c27c;font-size:.9rem;margin-left:.5rem}
.search{margin:1.5rem 0;display:flex;gap:.5rem}
.search input{flex:1;padding:.6rem;font-size:1rem;border:1px solid #444;border-radius:8px;background:#222;color:#eee}
.search button{padding:.6rem 1.2rem;font-size:1rem;border:none;border-radius:8px;background:#ec4141;color:#fff;cursor:pointer}
.search button:hover{background:#cc3030}
.song{padding:.8rem 1rem;margin:.3rem 0;background:#1a1a1a;border-radius:8px;cursor:pointer;display:flex;align-items:center;gap:1rem}
.song:hover{background:#252525}
.song .idx{color:#666;width:24px;text-align:center}
.song .name{font-weight:bold;font-size:1.05rem}
.song .info{color:#999;font-size:.85rem}
.song .dur{color:#666;font-size:.85rem;margin-left:auto;white-space:nowrap}
.song .src{font-size:.7rem;padding:2px 6px;border-radius:4px;margin-left:.5rem}
.src-netease{background:#ec4141;color:#fff}
.src-qq{background:#31c27c;color:#fff}
#player{margin-top:1rem;padding:1rem;background:#1a2a1a;border-radius:8px;display:none}
#player audio{width:100%;margin-top:.5rem}
.status{color:#31c27c;margin:1rem 0}
.error{color:#e74c3c;margin:1rem 0}
</style></head><body>
<h1>Music Proxy <span>Async · NetEase + QQ</span></h1>
<p>Online music source for cuckoo clock | Async IO, multi-device ready</p>
<div id="status-bar" style="display:flex;gap:1rem;margin:1rem 0;padding:.6rem 1rem;border-radius:8px;background:#1a2a1a;font-size:.85rem">
  <span id="proxy-status">Proxy: checking...</span>
  <span id="cache-info"></span>
</div>
<div class="search">
  <input id="q" placeholder="Search songs..." onkeydown="if(event.key==='Enter')search()">
  <button onclick="search()">Search</button>
</div>
<div id="results"></div>
<div id="player">
  <div id="now" style="color:#31c27c;font-weight:bold"></div>
  <audio id="audio" controls autoplay></audio>
</div>
<script>
(async function checkStatus(){
  try{
    const r=await fetch('/status');
    const d=await r.json();
    document.getElementById('proxy-status').innerHTML='Proxy: <span style="color:#31c27c">ONLINE</span> v'+d.version;
  }catch(e){
    document.getElementById('proxy-status').innerHTML='Proxy: <span style="color:#e74c3c">OFFLINE</span>';
    document.getElementById('status-bar').style.background='#3a1a1a';
  }
  try{
    const r=await fetch('/cache-info');
    const d=await r.json();
    document.getElementById('cache-info').textContent='Cache: '+d.count+' songs ('+d.size+')';
  }catch(e){
    document.getElementById('cache-info').textContent='Cache: N/A';
  }
})();
async function search(){
  const q=document.getElementById('q').value;
  if(!q)return;
  document.getElementById('results').innerHTML='<div class="status">Searching...</div>';
  try{
    const r=await fetch('/search?q='+encodeURIComponent(q)+'&limit=15');
    const d=await r.json();
    if(d.error){document.getElementById('results').innerHTML='<div class="error">'+d.error+'</div>';return}
    let html=d.songs.map((s,i)=>{
      const m=Math.floor(s.duration/60),sec=String(s.duration%60).padStart(2,'0');
      const src=s.source==='netease'?'NetEase':'QQ';
      const srcCls=s.source==='netease'?'src-netease':'src-qq';
      return '<div class="song" onclick="play(this)" data-sid="'+s.id+'" data-mid="'+(s.mid||'')+'" data-name="'+s.name.replace(/"/g,'&quot;')+'" data-source="'+s.source+'">'
        +'<span class="idx">'+(i+1)+'</span>'
        +'<div><div class="name">'+s.name+'<span class="src '+srcCls+'">'+src+'</span></div>'
        +'<div class="info">'+s.singer+' &middot; '+s.album+'</div></div>'
        +'<span class="dur">'+m+':'+sec+'</span></div>';
    }).join('');
    document.getElementById('results').innerHTML=html||'<div class="status">No results</div>';
  }catch(e){
    document.getElementById('results').innerHTML='<div class="error">Request failed</div>';
  }
}
async function play(el){
  const sid=el.dataset.sid,mid=el.dataset.mid,source=el.dataset.source,name=el.dataset.name;
  document.getElementById('now').textContent='Loading: '+name;
  document.getElementById('player').style.display='block';
  try{
    const params=new URLSearchParams();
    if(source==='netease')params.set('id',sid);
    else params.set('mid',mid);
    params.set('source',source);
    const r=await fetch('/play?'+params);
    const d=await r.json();
    if(d.error){document.getElementById('now').innerHTML='<span style="color:#e74c3c">'+d.error+'</span>';return}
    const audio=document.getElementById('audio');
    audio.src=d.url;
    audio.load();
    audio.play();
    document.getElementById('now').textContent=name;
  }catch(e){
    document.getElementById('now').innerHTML='<span style="color:#e74c3c">Failed</span>';
  }
}
</script>
</body></html>"""


async def handle_home(request):
    return web.Response(text=HOME_HTML, content_type="text/html; charset=utf-8")


async def handle_status(request):
    return web.json_response({"status": "ok", "version": "4.0-async"})


async def handle_cache_info(request):
    import glob
    cache_dir = os.path.join(os.path.dirname(__file__), "cache")
    loop = asyncio.get_event_loop()
    def _count():
        count = 0
        total_size = 0
        if os.path.isdir(cache_dir):
            for f in glob.glob(os.path.join(cache_dir, "*.mp3")):
                count += 1
                total_size += os.path.getsize(f)
        return count, total_size
    count, total_size = await loop.run_in_executor(None, _count)
    size_str = f"{total_size / 1024 / 1024:.1f} MB" if total_size >= 1048576 else f"{total_size:,} B"
    return web.json_response({"count": count, "size": size_str, "bytes": total_size})


async def handle_search(request):
    q = request.query.get("q", "")
    limit = int(request.query.get("limit", 10))
    if not q:
        return web.json_response({"error": "Missing param: q"})
    songs, err = await unified_search(q, limit)
    if err:
        return web.json_response({"error": err})
    return web.json_response({"songs": songs})


async def handle_play(request):
    source = request.query.get("source", "netease")
    if source == "netease":
        sid = request.query.get("id", "")
        if not sid:
            return web.json_response({"error": "Missing param: id"})
        url, err = await netease_play_url(int(sid))
    else:
        mid = request.query.get("mid", "")
        if not mid:
            return web.json_response({"error": "Missing param: mid"})
        url, err = await qq_play_url(mid)
    if err:
        return web.json_response({"error": err})
    return web.json_response({"url": url})


async def handle_quick(request):
    q = request.query.get("q", "")
    if not q:
        return web.json_response({"error": "Missing param: q"})
    songs, err = await unified_search(q, 3)
    if err or not songs:
        return web.json_response({"error": err or "No songs found"})
    for song in songs[:3]:
        url, e = await get_play_url_for(song)
        if url:
            return web.json_response({"url": url, "song": song})
    return web.json_response({"error": "Cannot play", "song": songs[0]})


# ---------------------------------------------------------------------------
# Audio streaming handlers (/stream, /pcm, /opus)
# ---------------------------------------------------------------------------

async def _stream_ffmpeg(keyword, fmt, request):
    """Stream audio via ffmpeg transcoding (async subprocess)."""
    _log(fmt, f"Searching: {keyword}")
    song_info, audio_url = await _find_playable(keyword)
    if song_info is None:
        raise web.HTTPNotFound(text=f"No result for: {keyword}")

    _log(fmt, f"Found: {song_info['name']} - {song_info['singer']}")

    ffmpeg_path = "ffmpeg"

    if fmt == "pcm":
        args = [
            ffmpeg_path, "-nostdin", "-loglevel", "error",
            "-user_agent", UA,
            "-reconnect", "1", "-reconnect_streamed", "1",
            "-i", audio_url,
            "-af", "aresample=16000:resampler=swr:osf=s16:dither_method=triangular",
            "-c:a", "pcm_s16le",
            "-f", "s16le",
            "-ar", "16000",
            "-ac", "1",
            "-"
        ]
        ct = "audio/raw"
    else:  # opus
        args = [
            ffmpeg_path, "-nostdin", "-loglevel", "error",
            "-user_agent", UA,
            "-reconnect", "1", "-reconnect_streamed", "1",
            "-i", audio_url,
            "-c:a", "libopus",
            "-b:a", "24k",
            "-ar", "16000",
            "-ac", "1",
            "-frame_duration", "60",
            "-application", "audio",
            "-f", "ogg",
            "-"
        ]
        ct = "audio/ogg; codecs=opus"

    proc = await asyncio.create_subprocess_exec(
        *args,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
    )

    resp = web.StreamResponse(status=200)
    resp.headers["Content-Type"] = ct
    resp.headers["X-Sample-Rate"] = "16000"
    try:
        resp.headers["X-Song-Name"] = song_info["name"].encode("ascii", errors="replace").decode("ascii")
        resp.headers["X-Song-Singer"] = song_info["singer"].encode("ascii", errors="replace").decode("ascii")
    except:
        pass
    await resp.prepare(request)

    total = 0
    try:
        while True:
            chunk = await proc.stdout.read(65536)
            if not chunk:
                break
            try:
                await resp.write(chunk)
                total += len(chunk)
            except ConnectionResetError:
                break
    except Exception as e:
        _log(fmt, f"Stream error: {e}")
    finally:
        # Cleanup ffmpeg on disconnect
        try:
            proc.stdout.close()
            proc.terminate()
            await asyncio.wait_for(proc.wait(), timeout=3)
        except:
            try:
                proc.kill()
            except:
                pass

    await resp.write_eof()
    _log(fmt, f"Done: {total} bytes")
    return resp


async def handle_stream(request):
    """Serve MP3 audio to ESP32 (streaming + cache)."""
    q = request.query.get("q", "")
    if not q:
        raise web.HTTPBadRequest(text="Missing param: q")

    _log("stream", f"Request: q={q}")
    songs, err = await unified_search(q, 5)
    if err or not songs:
        raise web.HTTPNotFound(text=f"No result for: {q}")

    song_info = songs[0]
    song_id = str(song_info.get("id", q))

    # Check cache
    if cached:
        _log("stream", f"CACHE HIT: {song_info['name']} - {song_info['singer']}")
        fsize = os.path.getsize(cached)
        headers = {
            "Content-Type": "audio/mpeg",
            "Content-Length": str(fsize),
        }
        try:
            headers["X-Song-Name"] = song_info["name"].encode("ascii", errors="replace").decode("ascii")
            headers["X-Song-Singer"] = song_info["singer"].encode("ascii", errors="replace").decode("ascii")
        except:
            pass
        return web.FileResponse(cached, headers=headers)

    # Stream from source
    song_info, audio_url = await _find_playable(q)
    if not audio_url:
        raise web.HTTPNotFound(text=f"Cannot play: {q}")

    _log("stream", f"Streaming: {song_info['name']} - {song_info['singer']}")

    # Use aiohttp to fetch upstream audio
    conn = aiohttp.TCPConnector(ssl=_ssl_ctx())
    async with aiohttp.ClientSession(connector=conn, timeout=aiohttp.ClientTimeout(total=120)) as sess:
        headers = {"User-Agent": UA, "Referer": "https://music.163.com"}
        async with sess.get(audio_url, headers=headers) as upstream:
            resp = web.StreamResponse(status=200)
            resp.headers["Content-Type"] = "audio/mpeg"
            try:
                resp.headers["X-Song-Name"] = song_info["name"].encode("ascii", errors="replace").decode("ascii")
                resp.headers["X-Song-Singer"] = song_info["singer"].encode("ascii", errors="replace").decode("ascii")
            except:
                pass
            await resp.prepare(request)

            total = 0
            tmp_path = os.path.join(CACHE_DIR, f".tmp_{song_id}")
            with open(tmp_path, 'wb') as tmpf:
                while True:
                    chunk = await upstream.content.read(262144)
                    if not chunk:
                        break
                    try:
                        await resp.write(chunk)
                        tmpf.write(chunk)
                        total += len(chunk)
                    except ConnectionResetError:
                        break

            await resp.write_eof()
            _log("stream", f"Sent {total} bytes OK (caching...)")

            # Atomic rename into cache
            try:
                os.replace(tmp_path, _cache_path(song_id))
            except OSError:
                if os.path.exists(tmp_path):
                    os.remove(tmp_path)

    return resp


async def handle_pcm(request):
    q = request.query.get("q", "")
    if not q:
        raise web.HTTPBadRequest(text="Missing ?q=")
    return await _stream_ffmpeg(q, "pcm", request)


async def handle_opus(request):
    q = request.query.get("q", "")
    if not q:
        raise web.HTTPBadRequest(text="Missing ?q=")
    return await _stream_ffmpeg(q, "opus", request)


# Rate limiting middleware
@web.middleware
async def rate_limit_middleware(request, handler):
    client_ip = request.remote
    if not _check_rate_limit(client_ip):
        return web.Response(status=429, text="Rate limit exceeded, retry later")
    async with _semaphore:
        return await handler(request)


# ===========================================================================
# Main
# ===========================================================================

def main():
    import socket
    try:
        local_ip = socket.gethostbyname(socket.gethostname())
    except:
        local_ip = "127.0.0.1"

    print("=" * 50)
    print("  Music Proxy Service v4.0-async (aiohttp)")
    print("  NetEase + QQ Music | Async IO")
    print("  Multi-device ready")
    print("=" * 50)
    print(f"\n  Web: http://{local_ip}:{PORT}")
    print(f"  API: http://{local_ip}:{PORT}/search?q=xxx")
    print(f"  Stream: http://{local_ip}:{PORT}/stream?q=xxx")
    print(f"  PCM: http://{local_ip}:{PORT}/pcm?q=xxx")
    print(f"  Cache: {CACHE_DIR}")
    print(f"\n  Press Ctrl+C to stop\n")

    _cache_cleanup()

    app = web.Application(middlewares=[rate_limit_middleware])
    app.router.add_get("/", handle_home)
    app.router.add_get("/index.html", handle_home)
    app.router.add_get("/status", handle_status)
    app.router.add_get("/cache-info", handle_cache_info)
    app.router.add_get("/search", handle_search)
    app.router.add_get("/play", handle_play)
    app.router.add_get("/quick", handle_quick)
    app.router.add_get("/stream", handle_stream)
    app.router.add_get("/pcm", handle_pcm)
    app.router.add_get("/opus", handle_opus)

    web.run_app(app, host="0.0.0.0", port=PORT)


if __name__ == "__main__":
    main()
