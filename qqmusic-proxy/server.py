# Music proxy service for ESP32 cuckoo clock
# Sources: NetEase Cloud Music (primary) + QQ Music (fallback)
# Start: python server.py  |  Listen: http://0.0.0.0:8765

import json
import time
import os
import subprocess
import urllib.request
import urllib.parse
import http.server
import socketserver
import ssl
import sys
import threading

PORT = 8765

# ===========================================================================
# Cantonese Dictionary (开放粤语字典)
# ===========================================================================
_cantonese_dict = None  # lazy-load on first request
_cantonese_lock = threading.Lock()

def _load_cantonese_dict():
    """Load Cantonese dictionary into memory. Returns list of dicts."""
    global _cantonese_dict
    with _cantonese_lock:
        if _cantonese_dict is not None:
            return _cantonese_dict
        path = os.path.join(os.path.dirname(__file__), "cantonese_dict.txt")
        if not os.path.exists(path):
            print("[cantonese] Dictionary file not found:", path)
            _cantonese_dict = []
            return _cantonese_dict
        entries = []
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split("\t")
                if len(parts) >= 3:
                    entry = {
                        "trad": parts[0],
                        "simp": parts[1],
                        "jyutping": parts[2],
                        "examples": parts[3] if len(parts) > 3 else "",
                        "definition": parts[4] if len(parts) > 4 else "",
                    }
                    entries.append(entry)
        _cantonese_dict = entries
        print(f"[cantonese] Loaded {len(entries)} dictionary entries")
        return _cantonese_dict

def _lookup_cantonese(word):
    """Look up a Chinese word/phrase in Cantonese dictionary.
    Returns a dict with results, or None if not found."""
    entries = _load_cantonese_dict()
    if not entries:
        return None
    # 按需建立搜索索引（延迟加载）—— 按字符匹配
    results = []
    for ch in word:
        for e in entries:
            if e["trad"] == ch or e["simp"] == ch:
                if e not in results:
                    results.append(e)
                break
    if not results:
        return None
    return {
        "word": word,
        "results": [
            {
                "char": r["trad"] if r["trad"] != r["simp"] else r["simp"],
                "jyutping": r["jyutping"],
                "definition": r["definition"][:120] if r["definition"] else "",
            }
            for r in results
        ],
    }
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"
CACHE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache")

# ===========================================================================
# 本地曲库 + 查询别名映射 + 搜索结果排序 (2026-07-19)
# ===========================================================================
LOCAL_MUSIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "local_music")
ALIASES_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "aliases.json")
_AUDIO_EXTS = (".mp3", ".m4a", ".flac", ".wav", ".ogg", ".aac")

import re as _re

def _normalize(s):
    """归一化: 去空白/标点/大小写, 用于模糊匹配"""
    return _re.sub(r"[\s\-_·,，。.．!！?？'\"“”‘’《》〈〉()（）\[\]【】+&/\\]+", "", str(s)).lower()

_aliases_state = {"mtime": -1.0, "subs": [], "qmap": []}

def load_aliases():
    """加载 aliases.json (mtime 变化时热重载)
    格式: {"substitutions": {"错词":"正词"}, "query_map": {"口语说法":"实际搜索词"}}"""
    try:
        mtime = os.path.getmtime(ALIASES_PATH) if os.path.exists(ALIASES_PATH) else 0
        if mtime != _aliases_state["mtime"]:
            subs, qmap = [], []
            if mtime:
                with open(ALIASES_PATH, "r", encoding="utf-8") as f:
                    data = json.load(f)
                subs = sorted(data.get("substitutions", {}).items(), key=lambda kv: -len(kv[0]))
                qmap = sorted(data.get("query_map", {}).items(), key=lambda kv: -len(kv[0]))
            _aliases_state.update(mtime=mtime, subs=subs, qmap=qmap)
            print(f"[alias] Loaded {len(subs)} substitutions, {len(qmap)} query mappings")
    except Exception as e:
        print(f"[alias] Load failed: {e}")
    return _aliases_state

def apply_aliases(q):
    """先做错词替换, 再检查整句映射 (包含式匹配, 长词优先)"""
    st = load_aliases()
    q2 = q
    for wrong, right in st["subs"]:
        if wrong in q2:
            q2 = q2.replace(wrong, right)
    nq = _normalize(q2)
    for key, target in st["qmap"]:
        if _normalize(key) in nq:
            if q2 != target:
                print(f"[alias] '{q}' -> '{target}'")
            return target
    if q2 != q:
        print(f"[alias] '{q}' -> '{q2}'")
    return q2

def local_search(keyword):
    """本地曲库匹配: 文件名(歌名-歌手)与查询词双向包含 / 全 token 命中"""
    hits = []
    try:
        if not os.path.isdir(LOCAL_MUSIC_DIR):
            return hits
        nq = _normalize(keyword)
        if not nq:
            return hits
        tokens = [_normalize(t) for t in keyword.split() if _normalize(t)]
        for fn in sorted(os.listdir(LOCAL_MUSIC_DIR)):
            stem, ext = os.path.splitext(fn)
            if ext.lower() not in _AUDIO_EXTS:
                continue
            ns = _normalize(stem)
            matched = ns in nq or nq in ns or (tokens and all(t in ns for t in tokens))
            if matched:
                name, singer = stem, "本地曲库"
                if "-" in stem:
                    parts = stem.split("-", 1)
                    name, singer = parts[0].strip(), parts[1].strip()
                hits.append({"name": name, "singer": singer, "source": "local",
                             "path": os.path.join(LOCAL_MUSIC_DIR, fn)})
    except Exception as e:
        print(f"[local] Search failed: {e}")
    return hits

def rank_songs(songs, keyword):
    """歌名+歌手都命中查询词的排前面 (稳定排序, 平台原序为次序)"""
    try:
        tokens = [_normalize(t) for t in keyword.split() if _normalize(t)]
        if len(tokens) < 2 or not songs:
            return songs
        def score(s):
            name_n = _normalize(s.get("name", ""))
            singer_n = _normalize(s.get("singer", ""))
            name_hits = sum(1 for t in tokens if t in name_n)
            singer_hits = sum(1 for t in tokens if t in singer_n)
            return (1 if (name_hits and singer_hits) else 0, 1 if name_hits else 0, name_hits + singer_hits)
        return sorted(songs, key=score, reverse=True)
    except Exception:
        return songs

# 确保缓存目录存在
os.makedirs(CACHE_DIR, exist_ok=True)

# 并发限制：服务器仅 1.6GB RAM，每个 ffmpeg 进程约占 50-100MB
# 最多允许 3 个并发流式请求，超出的排队等待
import threading
MAX_CONCURRENT = 5
_concurrency_sem = threading.Semaphore(MAX_CONCURRENT)

import datetime as _dt
import collections as _col

_LOG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")
os.makedirs(_LOG_DIR, exist_ok=True)

def log_platform(keyword, platform, action, result, extra=""):
    """Log platform search/play activity to daily CSV."""
    try:
        today = _dt.date.today().isoformat()
        path = os.path.join(_LOG_DIR, f"platform_{today}.csv")
        is_new = not os.path.exists(path)
        with open(path, "a", encoding="utf-8") as f:
            if is_new:
                f.write("timestamp,keyword,platform,action,result,details\n")
            ts = _dt.datetime.now().strftime("%H:%M:%S")
            kw = keyword.replace(",", " ").replace('"', "").strip()[:60]
            extra_s = str(extra).replace(",", " ").replace('"', "").replace("\n", " ")[:200]
            f.write(f"{ts},{kw},{platform},{action},{result},{extra_s}\n")
    except Exception:
        pass

def cleanup_old_logs(keep_days=30):
    """Remove log files older than keep_days."""
    try:
        cutoff = _dt.date.today() - _dt.timedelta(days=keep_days)
        for fn in os.listdir(_LOG_DIR):
            if fn.startswith("platform_") and fn.endswith(".csv"):
                try:
                    d = _dt.date.fromisoformat(fn[9:19])
                    if d < cutoff:
                        os.remove(os.path.join(_LOG_DIR, fn))
                except Exception:
                    pass
    except Exception:
        pass

cleanup_old_logs()


# ---------------------------------------------------------------------------
# SSL context (skip cert verification for some CDN servers)
# ---------------------------------------------------------------------------
def _ssl_ctx():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx

# ===========================================================================
# NetEase Cloud Music API
# ===========================================================================

def netease_search(keyword, limit=10):
    """Search NetEase Cloud Music"""
    params = urllib.parse.urlencode({
        "type": 1,     # single track
        "limit": limit,
        "s": keyword,
    })
    url = f"https://music.163.com/api/search/get?{params}"
    req = urllib.request.Request(url, headers={
        "User-Agent": UA,
        "Referer": "https://music.163.com",
    })
    try:
        with urllib.request.urlopen(req, timeout=10, context=_ssl_ctx()) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        return None, f"NetEase search failed: {e}"

    songs = []
    for item in data.get("result", {}).get("songs", []):
        sid = item.get("id", 0)
        name = item.get("name", "")
        artists = [a.get("name", "") for a in item.get("artists", [])]
        singer = "/".join(artists) if artists else "Unknown"
        album = item.get("album", {}).get("name", "Unknown")
        duration = item.get("duration", 0) // 1000  # ms -> s
        songs.append({
            "id": sid,
            "mid": str(sid),
            "name": name,
            "singer": singer,
            "album": album,
            "duration": duration,
            "source": "netease",
        })
    return songs, None


def netease_play_url(song_id, br=320000):
    """Get NetEase play URL"""
    url = f"https://music.163.com/api/song/enhance/player/url?id={song_id}&ids=[{song_id}]&br={br}"
    req = urllib.request.Request(url, headers={
        "User-Agent": UA,
        "Referer": "https://music.163.com",
    })
    try:
        with urllib.request.urlopen(req, timeout=10, context=_ssl_ctx()) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        return None, f"NetEase get play URL failed: {e}"

    song_data = data.get("data", [{}])[0]
    play_url = song_data.get("url", "")
    if not play_url:
        return None, "No playable URL (VIP-only or removed)"
    return play_url, None


# ===========================================================================
# QQ Music API (fallback)
# ===========================================================================

QQ_SEARCH_URL = "https://c.y.qq.com/soso/fcgi-bin/client_search_cp"
QQ_SONG_URL = "https://u.y.qq.com/cgi-bin/musicu.fcg"


def qq_search(keyword, limit=10):
    """Search QQ Music"""
    params = urllib.parse.urlencode({
        "format": "json", "w": keyword, "n": limit,
        "p": 1, "cr": 1, "type": 0, "new_json": 1,
    })
    url = f"{QQ_SEARCH_URL}?{params}"
    req = urllib.request.Request(url, headers={
        "User-Agent": UA, "Referer": "https://y.qq.com"
    })
    try:
        with urllib.request.urlopen(req, timeout=10, context=_ssl_ctx()) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        return None, f"QQ search failed: {e}"

    songs = []
    song_list = data.get("data", {}).get("song", {}).get("list", [])
    for item in song_list:
        mid = item.get("mid", "")
        name = item.get("name", "")
        singer_list = [s.get("name", "") for s in item.get("singer", [])]
        singer = "/".join(singer_list) if singer_list else "Unknown"
        album = item.get("album", {}).get("name", "Unknown")
        duration = item.get("interval", 0)
        songs.append({
            "id": mid,
            "mid": mid,
            "name": name,
            "singer": singer,
            "album": album,
            "duration": duration,
            "source": "qq",
        })
    return songs, None


def qq_play_url(song_mid):
    """Get QQ Music play URL"""
    guid = str(int(time.time() * 1000)) + "0" * 8
    payload = json.dumps({
        "req_0": {
            "module": "vkey.GetVkeyServer",
            "method": "CgiGetVkey",
            "param": {
                "guid": guid,
                "songmid": [song_mid],
                "songtype": [0],
                "uin": "0",
                "loginflag": 1,
                "platform": "20",
            }
        }
    }).encode()

    try:
        req = urllib.request.Request(QQ_SONG_URL, data=payload, headers={
            "User-Agent": UA, "Referer": "https://y.qq.com",
            "Content-Type": "application/json",
        })
        with urllib.request.urlopen(req, timeout=10, context=_ssl_ctx()) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        return None, f"QQ get play URL failed: {e}"

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
# 统一搜索+播放（聚合网易云+QQ+咪咕三个曲源）
# ===========================================================================

# ===========================================================================
# Migu Music API (3rd source, 2026-07-19)
# ===========================================================================

MIGU_SEARCH_URL = "https://pd.musicapp.migu.cn/MIGUM2.0/v1.0/content/search_all.do"
MIGU_LISTEN_URL = "https://app.pd.nf.migu.cn/MIGUM2.0/v1.0/content/sub/listenSong.do"
MIGU_UA = "Android_migu"

def migu_search(keyword, limit=10):
    """Search Migu Music (search_all.do)"""
    params = urllib.parse.urlencode({
        "ua": MIGU_UA, "version": "5.0.1",
        "pageNo": 1, "pageSize": limit,
        "text": keyword,
        "searchSwitch": '{"song":1}',
    })
    req = urllib.request.Request(f"{MIGU_SEARCH_URL}?{params}", headers={"User-Agent": MIGU_UA})
    try:
        with urllib.request.urlopen(req, timeout=10, context=_ssl_ctx()) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        return None, f"Migu search failed: {e}"

    songs = []
    try:
        result = (data.get("songResultData") or {}).get("result") or []
        for item in result:
            cid = item.get("contentId", "")
            crid = item.get("copyrightId", "")
            if not cid or not crid:
                continue
            name = _re.sub(r"（专辑：.*?）", "", item.get("name", "")).strip()
            singers = [s.get("name", "") for s in item.get("singers", [])]
            singer = "/".join([s for s in singers if s]) or "Unknown"
            albums = item.get("albums") or []
            album = albums[0].get("name", "") if albums else ""
            songs.append({
                "id": cid,
                "mid": cid,
                "contentId": cid,
                "copyrightId": crid,
                "name": name,
                "singer": singer,
                "album": album,
                "duration": 0,
                "source": "migu",
            })
    except Exception as e:
        return None, f"Migu parse failed: {e}"
    return songs, None


def migu_play_url(song):
    """Resolve Migu play URL via listenSong.do redirect (Range 0-0 probe)"""
    params = urllib.parse.urlencode({
        "toneFlag": "PQ", "netType": "00",
        "userId": "15548614588710179085069",
        "ua": MIGU_UA, "version": "5.1",
        "copyrightId": song.get("copyrightId", ""),
        "contentId": song.get("contentId", song.get("id", "")),
        "resourceType": "2", "channel": "0",
    })
    req = urllib.request.Request(f"{MIGU_LISTEN_URL}?{params}",
                                 headers={"User-Agent": MIGU_UA, "Range": "bytes=0-0"})
    try:
        with urllib.request.urlopen(req, timeout=10, context=_ssl_ctx()) as resp:
            final_url = resp.geturl()
            ctype = resp.headers.get("Content-Type", "")
        if "audio" in ctype or ".mp3" in final_url or "freetyst" in final_url:
            return final_url, None
        return None, f"Migu no audio URL (type={ctype})"
    except Exception as e:
        return None, f"Migu play URL failed: {e}"


def unified_search(keyword, limit=10):
    """Search Netease + QQ + Migu concurrently, merge results (dedup)"""
    import threading

    results = {"netease": (None, None), "qq": (None, None), "migu": (None, None)}

    def _search_netease():
        results["netease"] = netease_search(keyword, limit)

    def _search_qq():
        results["qq"] = qq_search(keyword, min(5, limit))

    def _search_migu():
        results["migu"] = migu_search(keyword, min(5, limit))

    threads = [threading.Thread(target=f, daemon=True) for f in (_search_netease, _search_qq, _search_migu)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=12)

    all_songs = []
    songs, err = results["netease"]
    if songs:
        all_songs.extend(songs)
    log_platform(keyword, "netease", "search", "ok" if songs else "fail", f"{len(songs) if songs else 0} results{'; '+str(err) if err else ''}")

    for src in ("qq", "migu"):
        songs2, err2 = results[src]
        if songs2:
            existing = {(s["name"], s["singer"]) for s in all_songs}
            for s in songs2:
                if (s["name"], s["singer"]) not in existing:
                    all_songs.append(s)
        log_platform(keyword, src, "search", "ok" if songs2 else "fail", f"{len(songs2) if songs2 else 0} results{'; '+str(err2) if err2 else ''}")

    if not all_songs:
        return None, "No results from any platform"

    return all_songs[:limit], None


def get_play_url_for(song):
    """Get play URL based on song source"""
    source = song.get("source", "qq")
    sid = song.get("id", song.get("mid", ""))
    kw = f"{song.get('name','')} {song.get('singer','')}"

    if source == "netease":
        url = netease_play_url(int(sid))
        log_platform(kw, "netease", "play", "ok" if url else "fail", f"id={sid}")
        return url
    elif source == "migu":
        url = migu_play_url(song)
        log_platform(kw, "migu", "play", "ok" if url else "fail", f"sid={sid}")
        return url
    else:
        url = qq_play_url(song.get("mid", sid))
        log_platform(kw, "qq", "play", "ok" if url else "fail", f"mid={song.get('mid',sid)}")
        return url


# ===========================================================================
# 本地磁盘缓存（下载后存为MP3文件，下次直接读文件）
# ===========================================================================

def _cache_path(song_id):
    """Get cache file path for a song ID"""
    safe_id = str(song_id).replace("/", "_").replace("\\", "_")
    return os.path.join(CACHE_DIR, f"{safe_id}.mp3")

def _cache_get(song_id):
    """Check if song exists in cache and is >0 bytes"""
    path = _cache_path(song_id)
    if os.path.exists(path) and os.path.getsize(path) > 0:
        return path
    return None

def _cache_put(song_id, data):
    """Save song to cache"""
    path = _cache_path(song_id)
    try:
        with open(path, "wb") as f:
            f.write(data)
        print(f"[cache] Saved {len(data)} bytes -> {path}")
    except Exception as e:
        print(f"[cache] Failed to save: {e}")


# ===========================================================================
# HTTP Service
# ===========================================================================

class MusicProxyHandler(http.server.BaseHTTPRequestHandler):

    def log_message(self, format, *args):
        print(f"[{time.strftime('%H:%M:%S')}] {args[0]}")

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        params = dict(urllib.parse.parse_qsl(parsed.query))

        try:
            if path in ("/", "/index.html"):
                self._serve_home()
            elif path == "/cantonese":
                self._handle_cantonese(params)
            elif path == "/search":
                self._handle_search(params)
            elif path == "/play":
                self._handle_play(params)
            elif path == "/quick":
                self._handle_quick(params)
            elif path == "/stream":
                self._handle_stream(params)
            elif path == "/pcm":
                self._handle_pcm(params)
            elif path == "/opus":
                self._handle_opus(params)
            elif path == "/status":
                self._json({"status": "ok", "version": "3.0"})
            elif path == "/cache-info":
                self._cache_info()
            else:
                self.send_error(404)
        except Exception as e:
            self.send_error(500, str(e))

    def _serve_home(self):
        html = r"""<!DOCTYPE html>
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
<h1>Music Proxy <span>NetEase + QQ</span></h1>
<p>Online music source for cuckoo clock | NetEase primary, QQ fallback</p>
<div id="status-bar" style="display:flex;gap:1rem;margin:1rem 0;padding:.6rem 1rem;border-radius:8px;background:#1a2a1a;font-size:.85rem">
  <span id="proxy-status">Proxy: checking...</span>
  <span id="cache-info"></span>
  <span id="proxy-uptime"></span>
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
    document.getElementById('proxy-uptime').textContent='Port: 8765';
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
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.end_headers()
        self.wfile.write(html.encode("utf-8"))

    def _handle_search(self, params):
        q = params.get("q", "")
        limit = int(params.get("limit", 10))
        if not q:
            return self._json({"error": "Missing param: q"})
        q = apply_aliases(q)
        local_hits = local_search(q)
        songs, err = unified_search(q, limit)
        songs = rank_songs(songs or [], q)
        merged = local_hits + songs
        if not merged:
            self._json({"error": err or "No results"})
        else:
            self._json({"songs": merged[:limit], "query": q})

    def _handle_play(self, params):
        source = params.get("source", "netease")
        if source == "netease":
            sid = params.get("id", "")
            if not sid:
                return self._json({"error": "Missing param: id"})
            url, err = netease_play_url(int(sid))
        else:
            mid = params.get("mid", "")
            if not mid:
                return self._json({"error": "Missing param: mid"})
            url, err = qq_play_url(mid)

        if err:
            self._json({"error": err})
        else:
            self._json({"url": url})

    def _handle_quick(self, params):
        """One-click search + return best play URL"""
        q = params.get("q", "")
        if not q:
            return self._json({"error": "Missing param: q"})

        songs, err = unified_search(q, 3)
        if err or not songs:
            return self._json({"error": err or "No songs found"})

        # 逐个尝试每首歌的URL，找到第一个能播的
        for song in songs[:3]:
            url, e = get_play_url_for(song)
            if url:
                return self._json({"url": url, "song": song})
        err_detail = get_play_url_for(songs[0])[1] or "Cannot play"
        return self._json({"error": err_detail, "song": songs[0]})

    def _handle_cantonese(self, params):
        """Cantonese dictionary lookup endpoint.
        GET /cantonese?word=X  →  JSON with jyutping + definitions.
        Used by ESP32 MCP tool cuckoo.cantonese_lookup."""
        word = params.get("word", "")
        if not word:
            return self._json({"error": "Missing param: word"})
        result = _lookup_cantonese(word)
        if result is not None:
            return self._json(result)
        # Compound phrase not in dictionary: split into chars, look up individually
        if len(word) > 1:
            chars = []
            for ch in word:
                r = _lookup_cantonese(ch)
                if r:
                    chars.append(r.get("jyutping", "?"))
                else:
                    chars.append("?")
            if any(c != "?" for c in chars):
                self._json({
                    "word": word,
                    "jyutping": " ".join(chars),
                    "definition": "char-by-char lookup",
                    "chars": chars,
                })
                return
        self._json({"error": "No results", "word": word})

    def _cache_info(self):
        import os, glob
        cache_dir = os.path.join(os.path.dirname(__file__), "cache")
        count = 0
        total_size = 0
        if os.path.isdir(cache_dir):
            for f in glob.glob(os.path.join(cache_dir, "*.mp3")):
                count += 1
                total_size += os.path.getsize(f)
        if total_size < 1024 * 1024:
            size_str = f"{total_size:,} B"
        else:
            size_str = f"{total_size / 1024 / 1024:.1f} MB"
        self._json({"count": count, "size": size_str, "bytes": total_size})

    def _json(self, data):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    # -----------------------------------------------------------------------
    # /stream - Audio proxy for ESP32 (streaming + cache)
    # -----------------------------------------------------------------------
    def _handle_stream(self, params):
        """Serve MP3 audio to ESP32.
        v4: Send HTTP headers immediately (chunked), then search+download.
        This prevents ESP32's 15s HTTP timeout during the search phase."""
        q = params.get("q", "")
        if not q:
            self.send_error(400, "Missing param: q")
            return

        # 并发限制
        if not _concurrency_sem.acquire(blocking=False):
            self.send_error(503, "Server busy, try again later")
            return
        try:
            self._handle_stream_inner(params, q)
        finally:
            _concurrency_sem.release()

    def _handle_stream_inner(self, params, q):

        print(f"[stream] Request: q={q}")

        q = apply_aliases(q)
        # 本地曲库命中时直接发文件
        local_hits = local_search(q)
        if local_hits:
            print(f"[stream] LOCAL HIT: {local_hits[0]['name']} - {local_hits[0]['singer']}")
            self._send_file(local_hits[0]["path"], local_hits[0])
            return

        # 搜索一次，同时用于缓存检查和后续播放
        songs, err = unified_search(q, 5)
        songs = rank_songs(songs or [], q)
        
        # 多版本检测：同一歌名有多个不同歌手时，返回选项让AI询问用户
        if songs and len(songs) >= 2:
            top_name = _normalize(songs[0].get("name", ""))
            artists = []
            seen = set()
            for s in songs[:5]:
                sn = _normalize(s.get("name", ""))
                sa = s.get("singer", "")
                # 模糊匹配：去除括号内容后比较，Cover/翻唱也算同一首歌
                import re as _re2
                sn_clean = _re2.sub(r'[\(（][^)）]*[\)）]', '', sn).strip()
                top_clean = _re2.sub(r'[\(（][^)）]*[\)）]', '', top_name).strip()
                if (sn_clean == top_clean or top_clean in sn_clean or sn_clean in top_clean) and sa and sa not in seen:
                    seen.add(sa)
                    artists.append(sa)
            if len(artists) >= 2:
                # 跳过已有别名映射的查询（如"请跟我来"→苏芮）
                import re as _re3
                st = load_aliases()
                is_aliased = False
                for key, _ in st.get("qmap", []):
                    if _re3.search(_normalize(key), _normalize(q)):
                        is_aliased = True
                        break
                if not is_aliased:
                    self.send_response(300)
                    self.send_header("Content-Type", "application/json; charset=utf-8")
                    self.send_header("X-Multi-Artist", "1")
                    resp = json.dumps({"multi_artist": True, "song": songs[0].get("name", keyword), "artists": artists, "hint": "\u591a\u4e2a\u6b4c\u624b\u6f14\u5531\u4e86\u8fd9\u9996\u6b4c\uff0c\u8bf7\u544a\u8bc9\u7528\u6237\u9009\u62e9\u4e00\u4f4d\u518d\u70b9\u6b4c"}, ensure_ascii=False).encode("utf-8")
                    self.send_header("Content-Length", str(len(resp)))
                    self.end_headers()
                    self.wfile.write(resp)
                    print(f"[stream] MULTI-ARTIST: {top_name} by {artists}")
                    return
        
        if songs:
            song_id = str(songs[0].get("id", q))
            cached = _cache_get(song_id)
            if cached:
                print(f"[stream] CACHE HIT: {songs[0]['name']} - {songs[0]['singer']}")
                self._send_file(cached, songs[0])
                return
        else:
            self.send_error(404, f"No result for: " + "{q".encode("ascii","replace").decode())
            return

        # === v4: 立即发送HTTP头（分块传输编码）===
        # 这样ESP32的HTTP客户端在搜索下载期间不会超时断开
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()

        # 现在搜索并获取可播放URL（ESP32没有超时，在等待数据）
        try:
            # 复用上面已搜索的结果，不再重复搜索
            audio_url = None
            song_info = None
            for song in songs[:5]:
                url, e = get_play_url_for(song)
                if url:
                    audio_url = url
                    song_info = song
                    break

            if not audio_url:
                self._send_chunk(b'')  # 发送空块，结束分块响应
                return

            print(f"[stream] Streaming: {song_info['name']} - {song_info['singer']}")

            req = urllib.request.Request(audio_url, headers={
                "User-Agent": UA,
                "Referer": "https://music.163.com",
            })
            ctx = _ssl_ctx()
            # 流式写入临时文件，避免整首歌缓存在内存中
            import tempfile
            tmp_fd, tmp_path = tempfile.mkstemp(suffix=".tmp", dir=CACHE_DIR)
            total_sent = 0
            try:
                with os.fdopen(tmp_fd, "wb") as tmp_f:
                    with urllib.request.urlopen(req, timeout=120, context=ctx) as upstream:
                        while True:
                            chunk = upstream.read(262144)  # 每次读取256KB
                            if not chunk:
                                break
                            self._send_chunk(chunk)
                            tmp_f.write(chunk)
                            total_sent += len(chunk)

                    # End chunked transfer
                    self._send_chunk(b'')
                    print(f"[stream] Sent {total_sent} bytes OK (caching...)")

                # 写完后 rename 到正式缓存路径
                song_id = str(song_info.get("id", song_info.get("mid", q)))
                cache_path = _cache_path(song_id)
                os.rename(tmp_path, cache_path)
                print(f"[cache] Saved {total_sent} bytes -> {cache_path}")
            except Exception as cache_err:
                # 写入失败删除临时文件
                try:
                    os.unlink(tmp_path)
                except:
                    pass
                raise

        except Exception as e:
            print(f"[stream] Error: {e}")
            try:
                self._send_chunk(b'')  # 发送空块，结束分块响应
            except:
                pass

    def _send_chunk(self, data):
        """Send a chunked transfer-encoding chunk"""
        if data:
            self.wfile.write(f"{len(data):X}\r\n".encode())
            self.wfile.write(data)
            self.wfile.write(b"\r\n")
        else:
            self.wfile.write(b"0\r\n\r\n")

    def _send_file(self, filepath, song_info):
        """Send a cached file to client"""
        try:
            fsize = os.path.getsize(filepath)
            self.send_response(200)
            self.send_header("Content-Type", "audio/mpeg")
            self.send_header("Content-Length", str(fsize))
            if song_info:
                self.send_header("X-Song-Name",
                    song_info["name"].encode("ascii", errors="replace").decode("ascii"))
                self.send_header("X-Song-Singer",
                    song_info["singer"].encode("ascii", errors="replace").decode("ascii"))
            self.end_headers()

            with open(filepath, "rb") as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            print(f"[stream] Cached: {fsize} bytes sent instantly")
        except Exception as e:
            print(f"[stream] Cache send error: {e}")

    # -----------------------------------------------------------------------
    # /pcm - 通过ffmpeg实时转码为原始PCM音频流
    # -----------------------------------------------------------------------
    def _handle_pcm(self, params):
        keyword = params.get("q", "")
        if not keyword:
            self.send_error(400, "Missing ?q=")
            return
        self._stream_ffmpeg(keyword, "pcm")

    # -----------------------------------------------------------------------
    # /opus - 通过ffmpeg实时转码为Opus OGG音频流
    # -----------------------------------------------------------------------
    def _handle_opus(self, params):
        keyword = params.get("q", "")
        if not keyword:
            self.send_error(400, "Missing ?q=")
            return
        self._stream_ffmpeg(keyword, "opus")

    def _find_playable(self, keyword):
        """Search and find a playable song URL. Returns (song_info, audio_url) or (None, None)."""
        # 本地曲库优先（命中即直接播本地文件，保底不依赖在线曲库）
        local_hits = local_search(keyword)
        if local_hits:
            print(f"[local] HIT: {local_hits[0]['name']} - {local_hits[0]['singer']}")
            return local_hits[0], local_hits[0]["path"]
        songs, err = unified_search(keyword, 8)
        if err or not songs:
            return None, None
        songs = rank_songs(songs, keyword)
        for song in songs[:5]:
            url, e = get_play_url_for(song)
            if url:
                return song, url
        return None, None

    def _stream_ffmpeg(self, keyword, fmt="pcm"):
        """Stream audio via ffmpeg transcoding"""

        # 并发限制
        if not _concurrency_sem.acquire(blocking=False):
            self.send_error(503, "Server busy, try again later")
            return
        try:
            self._stream_ffmpeg_inner(keyword, fmt)
        finally:
            _concurrency_sem.release()

    def _stream_ffmpeg_inner(self, keyword, fmt="pcm"):

        keyword = apply_aliases(keyword)
        print(f"[{fmt}] Searching: {keyword}")

        # 含空格查询已指定歌手，跳过多版本检测直接播
        if " " not in keyword:
            # 多版本检测：先搜索，同一首歌有多个歌手版本时返回选项
            try:
                songs, _ = unified_search(keyword, 6)
                songs = rank_songs(songs or [], keyword)
                if songs and len(songs) >= 2:
                    import re as _re_multi
                    top_name = _normalize(songs[0].get("name", ""))
                    artists = []
                    seen = set()
                    for s in songs[:6]:
                        sn = _normalize(s.get("name", ""))
                        sa = s.get("singer", "")
                        sn_clean = _re_multi.sub(r'[\(（][^)）]*[\)）]', '', sn).strip()
                        top_clean = _re_multi.sub(r'[\(（][^)）]*[\)）]', '', top_name).strip()
                        if (sn_clean == top_clean or top_clean in sn_clean or sn_clean in top_clean) and sa and sa not in seen:
                            seen.add(sa)
                            artists.append(sa)
                    if len(artists) >= 2:
                        from urllib.parse import quote as url_quote
                        song_name = songs[0].get("name", keyword)
                        kw_norm = _normalize(keyword)
                        sn_only = _re_multi.sub(r'[\(（][^)）]*[\)）]', '', _normalize(song_name)).strip()
                        if kw_norm not in sn_only and sn_only not in kw_norm:
                            artist_list = "、".join(artists)
                            hint = f"您搜的是'{keyword}'，曲库中最匹配的是《{song_name}》，有{len(artists)}个版本：{artist_list}。请告知AI向用户说明实际歌名，并让其选择演唱者。"
                        else:
                            artist_list = "、".join(artists)
                            hint = f"《{song_name}》有多个歌手版本：{artist_list}。请让用户选择一位演唱者。"
                        self._json({
                            "multi_artist": True,
                            "song": song_name,
                            "artists": artists,
                            "hint": hint
                        })
                        print(f"[{fmt}] MULTI-ARTIST: {top_name} by {artists}")
                        return
            except Exception as e:
                print(f"[{fmt}] multi-artist check skipped: {e}")
    
        # 单版本或无指定歌手，直接播放
        song_info, audio_url = self._find_playable(keyword)
        # verify artist match when user specified one
        if song_info is not None and " " in keyword:
            requested_artist = keyword.split(" ", 1)[1]
            found_singer = song_info.get("singer", "")
            if _normalize(requested_artist) not in _normalize(found_singer):
                print(f"[{fmt}] MISMATCH: requested={requested_artist}, got={found_singer}, triggering fallback")
                song_info = None
        if song_info is None:
            # 如果查询含空格（指定了歌手但找不到），回退搜多版本供用户重选
            if " " in keyword:
                try:
                    song_name = keyword.split(" ", 1)[0]
                    songs, _ = unified_search(song_name, 6)
                    songs = rank_songs(songs or [], song_name)
                    if songs and len(songs) >= 2:
                        import re as _re_fb
                        top_name = _normalize(songs[0].get("name", ""))
                        artists = []
                        seen = set()
                        for s in songs[:6]:
                            sn = _normalize(s.get("name", ""))
                            sa = s.get("singer", "")
                            sn_clean = _re_fb.sub(r'[\(\)][^)]*[)]', '', sn).strip()
                            top_clean = _re_fb.sub(r'[\(\)][^)]*[)]', '', top_name).strip()
                            if (sn_clean == top_clean or top_clean in sn_clean or sn_clean in top_clean) and sa and sa not in seen:
                                seen.add(sa)
                                artists.append(sa)
                        if len(artists) >= 2:
                            requested = keyword.split(" ", 1)[1]
                            requested_norm = _normalize(requested)
                            # Remove the unavailable artist from the list
                            filtered = [a for a in artists if _normalize(a) != requested_norm]
                            if len(filtered) < 2:
                                filtered = artists  # fallback: keep original list
                            song_name2 = songs[0].get("name", keyword)
                            artist_list2 = "、".join(filtered)
                            hint2 = f"抱歉，{requested} 的版本无法播放！曲库中《{song_name2}》可用版本：{artist_list2}。请让用户重新选择。"
                            self._json({
                                "multi_artist": True,
                                "song": song_name2,
                                "artists": filtered,
                                "hint": hint2
                            })
                            print(f"[{fmt}] UNAVAILABLE: {requested}, alternatives: {filtered}")
                            return
                except Exception as e:
                    print(f"[{fmt}] unavailable fallback failed: {e}")
            self.send_error(404, f"No result for: " + "{keyword".encode("ascii","replace").decode())
            return
        print(f"[{fmt}] Found: {song_info['name']} - {song_info['singer']}")

        try:
            ffmpeg_path = "/usr/bin/ffmpeg"
            if not os.path.exists(ffmpeg_path):
                ffmpeg_path = "ffmpeg"

            RATE_LIMIT = 65536  # 64KB/s ≈ 2x realtime for 16kHz mono s16le
            SUB_CHUNK = 2048    # 限速粒度

            # 本地文件不需要网络相关的输入参数
            if song_info.get("source") == "local":
                net_opts = []
            else:
                net_opts = ["-user_agent", UA, "-reconnect", "1", "-reconnect_streamed", "1"]

            if fmt == "pcm":
                args = [ffmpeg_path, "-nostdin", "-loglevel", "error"] + net_opts + [
                    "-i", audio_url,
                    "-af", "volume=1.5,dynaudnorm=f=500:g=10:p=0.95:m=16,aresample=16000:resampler=swr:osf=s16:dither_method=none",
                    "-c:a", "pcm_s16le",
                    "-f", "s16le",
                    "-ar", "16000",
                    "-ac", "1",
                    "-"
                ]
                ct = "audio/raw"
            else:  # opus
                args = [ffmpeg_path, "-nostdin", "-loglevel", "error"] + net_opts + [
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

            proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

            self.send_response(200)
            self.send_header("Content-Type", ct)
            self.send_header("X-Sample-Rate", "16000")
            self.send_header("X-Song-Name",
                song_info["name"].encode("ascii", errors="replace").decode("ascii"))
            self.send_header("X-Song-Singer",
                song_info["singer"].encode("ascii", errors="replace").decode("ascii"))
            self.end_headers()

            # 限速发送：前 128KB 双倍速（warmup 快速填缓冲），之后正常速
            total = 0
            warmup = True
            target_time = time.monotonic()
            client_gone = False
            while True:
                chunk = proc.stdout.read(65536)
                if not chunk:
                    break
                try:
                    for i in range(0, len(chunk), SUB_CHUNK):
                        piece = chunk[i:i+SUB_CHUNK]
                        rate = RATE_LIMIT * 2 if warmup else RATE_LIMIT
                        target_time += len(piece) / rate
                        now = time.monotonic()
                        if target_time > now:
                            time.sleep(target_time - now)
                        self.wfile.write(piece)
                        self.wfile.flush()
                    total += len(chunk)
                    if warmup and total >= 131072:
                        warmup = False
                except OSError:
                    client_gone = True  # ESP32 disconnected
                    break

            # Always close stdout + terminate proc to prevent zombie ffmpeg
            try:
                proc.stdout.close()
            except:
                pass
            try:
                proc.terminate()
                proc.wait(timeout=3)
            except:
                proc.kill()
            if client_gone:
                print(f"[{fmt}] Client disconnected, ffmpeg killed ({total} bytes sent)")
            else:
                print(f"[{fmt}] Done: {total} bytes")

        except OSError:
            print(f"[{fmt}] Client disconnected early")
        except Exception as e:
            print(f"[{fmt}] Error: {e}")
            import traceback
            traceback.print_exc()


# ===========================================================================
# Main
# ===========================================================================

def main():
    server = socketserver.ThreadingTCPServer(("0.0.0.0", PORT), MusicProxyHandler)
    server.allow_reuse_address = True

    import socket
    try:
        local_ip = socket.gethostbyname(socket.gethostname())
    except:
        local_ip = "127.0.0.1"

    print("=" * 50)
    print("  Music Proxy Service v3.1 (NetEase + QQ + Migu + Local)")
    print("  Cuckoo Clock Online Music")
    print("  FEATURES: streaming + local cache + ASCII-only")
    print("=" * 50)
    print(f"\n  Web: http://{local_ip}:{PORT}")
    print(f"  API: http://{local_ip}:{PORT}/search?q=xxx")
    print(f"  Stream: http://{local_ip}:{PORT}/stream?q=xxx")
    print(f"  Cache: {CACHE_DIR}")
    print(f"\n  Press Ctrl+C to stop\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n  Service stopped")
        server.shutdown()


if __name__ == "__main__":
    main()
