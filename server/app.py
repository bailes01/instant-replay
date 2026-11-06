#!/usr/bin/env python3
"""
Lightweight self-hosted clip share server (zero third-party Python dependencies; uses ffmpeg for backend trim).
Stores clips in DATA_DIR, associates per-user client tokens with usernames, supports Public/Private visibility,
and provides a visual dual-handle video trimmer with Save as Copy / Overwrite on the backend.
"""
from datetime import datetime
from http import cookies
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse
import html
import json
import os
import re
import secrets
import shutil
import subprocess
import threading
import time


def _load_dotenv():
    env_file = Path(__file__).resolve().parent / ".env"
    if env_file.is_file():
        for line in env_file.read_text("utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            os.environ.setdefault(k.strip(), v.strip())


_load_dotenv()


def _default_data_dir() -> str:
    d = os.environ.get("DATA_DIR", "./data")
    if os.name != "nt" and len(d) >= 3 and d[1] == ":" and d[2] in ("/", "\\"):
        drive = d[0].lower()
        d = f"/mnt/{drive}/{d[3:]}"
    return d


PORT = int(os.environ.get("PORT", "8080"))
PUBLIC_URL = os.environ.get("PUBLIC_URL", f"http://localhost:{PORT}").rstrip("/")
UPLOAD_TOKEN = os.environ.get("UPLOAD_TOKEN", "")
DATA_DIR = Path(_default_data_dir()).resolve()
MAX_UPLOAD_BYTES = int(os.environ.get("MAX_UPLOAD_MB", "2048")) * 1024 * 1024
MAX_STORAGE_BYTES = int(os.environ.get("MAX_STORAGE_GB", "50")) * 1024 * 1024 * 1024

ID_RE = re.compile(r"^[A-Za-z0-9_-]{4,32}$")
USER_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{8,128}$")
DATA_DIR.mkdir(parents=True, exist_ok=True)

USERS_LOCK = threading.Lock()
USERS_FILE = DATA_DIR / "users.json"


def _load_users() -> dict[str, str]:
    if not USERS_FILE.is_file():
        return {}
    try:
        return json.loads(USERS_FILE.read_text("utf-8"))
    except Exception:
        return {}


def _save_users(users: dict[str, str]):
    tmp = DATA_DIR / ".users.json.tmp"
    tmp.write_text(json.dumps(users, indent=2), encoding="utf-8")
    shutil.move(str(tmp), str(USERS_FILE))


def verify_or_register_user_token(username: str, token: str) -> bool:
    """
    Associates `token` with `username` if `username` does not have a token yet,
    or verifies that `token` matches the existing token for `username`.
    """
    user = sanitize_username(username)
    tok = (token or "").strip()
    if not user or not TOKEN_RE.match(tok):
        return False
    with USERS_LOCK:
        users = _load_users()
        existing = users.get(user)
        if not existing:
            users[user] = tok
            _save_users(users)
            return True
        return secrets.compare_digest(existing, tok)


def check_user_token_readonly(username: str, token: str) -> bool:
    user = sanitize_username(username)
    tok = (token or "").strip()
    if not user or not TOKEN_RE.match(tok):
        return False
    with USERS_LOCK:
        users = _load_users()
        existing = users.get(user)
        if not existing:
            return False
        return secrets.compare_digest(existing, tok)


def format_timestamp(ts: int) -> str:
    dt = datetime.fromtimestamp(ts)
    hour12 = dt.hour % 12 or 12
    ampm = "AM" if dt.hour < 12 else "PM"
    return f"{dt.strftime('%b')} {dt.day}, {dt.year} \u2022 {hour12}:{dt.strftime('%M')} {ampm}"


def sanitize_username(raw: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_-]", "", (raw or "").strip())[:32]
    return cleaned.lower() if cleaned else "default"


def sanitize_app_name(raw: str) -> str:
    cleaned = re.sub(r"[\r\n\t]+", " ", (raw or "").strip())[:80]
    return cleaned if cleaned else "Desktop Capture"


def enforce_storage_quota():
    mp4_files = sorted(DATA_DIR.glob("*.mp4"), key=lambda p: p.stat().st_mtime)
    total = sum(p.stat().st_size for p in mp4_files)
    while total > MAX_STORAGE_BYTES and len(mp4_files) > 1:
        oldest = mp4_files.pop(0)
        total -= oldest.stat().st_size
        clip_id = oldest.stem
        oldest.unlink(missing_ok=True)
        (DATA_DIR / f"{clip_id}.json").unlink(missing_ok=True)


def load_clip_meta(clip_id: str) -> dict | None:
    if not ID_RE.match(clip_id):
        return None
    mp4_path = DATA_DIR / f"{clip_id}.mp4"
    meta_path = DATA_DIR / f"{clip_id}.json"
    if not mp4_path.is_file():
        return None
    meta = {}
    if meta_path.is_file():
        try:
            meta = json.loads(meta_path.read_text("utf-8"))
        except Exception:
            meta = {}
    created_ts = int(meta.get("created_at", int(mp4_path.stat().st_mtime)))
    user = sanitize_username(meta.get("username", "default"))
    app_name = sanitize_app_name(meta.get("app_name", "Desktop Capture"))
    title = sanitize_app_name(meta.get("title", app_name))
    is_public = bool(meta.get("public", False))
    mtime = int(mp4_path.stat().st_mtime)
    return {
        "id": clip_id,
        "username": user,
        "app_name": app_name,
        "title": title,
        "public": is_public,
        "size": mp4_path.stat().st_size,
        "created_at": created_ts,
        "mtime": mtime,
        "formatted_date": format_timestamp(created_ts),
        "url": f"{PUBLIC_URL}/v/{clip_id}",
        "gallery": f"{PUBLIC_URL}/u/{user}",
        "raw": f"{PUBLIC_URL}/f/{clip_id}.mp4",
    }


def save_clip_meta(meta: dict):
    clip_id = meta["id"]
    meta_path = DATA_DIR / f"{clip_id}.json"
    tmp_path = DATA_DIR / f".{clip_id}.json.tmp"
    tmp_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")
    shutil.move(str(tmp_path), str(meta_path))


def load_all_clips(username_filter: str | None = None, include_private: bool = False) -> list[dict]:
    clips = []
    for meta_file in DATA_DIR.glob("*.json"):
        if meta_file.name == "users.json":
            continue
        clip_id = meta_file.stem
        c = load_clip_meta(clip_id)
        if not c:
            continue
        if username_filter and c["username"] != username_filter.lower():
            continue
        if not include_private and not c["public"]:
            continue
        clips.append(c)
    clips.sort(key=lambda x: x.get("created_at", 0), reverse=True)
    return clips


def ensure_thumbnail(clip_id: str, mp4_path: Path) -> Path | None:
    jpg_path = DATA_DIR / f"{clip_id}.jpg"
    if jpg_path.is_file() and jpg_path.stat().st_mtime >= mp4_path.stat().st_mtime:
        return jpg_path
    ffmpeg_bin = shutil.which("ffmpeg") or "/usr/bin/ffmpeg"
    for seek_s in ("0.5", "0.0"):
        cmd = [
            ffmpeg_bin, "-y",
            "-ss", seek_s,
            "-i", str(mp4_path),
            "-frames:v", "1",
            "-vf", "scale=640:-2",
            "-q:v", "5",
            str(jpg_path),
        ]
        try:
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
            if res.returncode == 0 and jpg_path.is_file() and jpg_path.stat().st_size > 200:
                return jpg_path
        except Exception:
            pass
    return None


def remux_faststart_interleaved(mp4_path: Path):
    ffmpeg_bin = shutil.which("ffmpeg") or "/usr/bin/ffmpeg"
    tmp_remux = mp4_path.with_suffix(".remux.mp4")
    cmd = [
        ffmpeg_bin, "-y", "-v", "error",
        "-i", str(mp4_path),
        "-c", "copy",
        "-movflags", "+faststart",
        str(tmp_remux),
    ]
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        if res.returncode == 0 and tmp_remux.is_file() and tmp_remux.stat().st_size > 1024:
            shutil.move(str(tmp_remux), str(mp4_path))
        else:
            tmp_remux.unlink(missing_ok=True)
    except Exception:
        tmp_remux.unlink(missing_ok=True)


def run_ffmpeg_trim(src_mp4: Path, dst_mp4: Path, start_sec: float, end_sec: float) -> bool:
    duration = max(0.2, end_sec - start_sec)
    ffmpeg_bin = shutil.which("ffmpeg") or "/usr/bin/ffmpeg"
    # First try stream-copy (instant, lossless, sub-100ms)
    cmd_copy = [
        ffmpeg_bin, "-y",
        "-ss", f"{start_sec:.3f}",
        "-i", str(src_mp4),
        "-t", f"{duration:.3f}",
        "-c", "copy",
        "-avoid_negative_ts", "make_zero",
        "-movflags", "+faststart",
        str(dst_mp4),
    ]
    res = subprocess.run(cmd_copy, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    if res.returncode == 0 and dst_mp4.is_file() and dst_mp4.stat().st_size > 1024:
        return True

    # Fallback: re-encode if stream copy failed on a sub-GOP cut
    cmd_enc = [
        ffmpeg_bin, "-y",
        "-ss", f"{start_sec:.3f}",
        "-i", str(src_mp4),
        "-t", f"{duration:.3f}",
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
        "-c:a", "aac", "-b:a", "160k",
        "-movflags", "+faststart",
        str(dst_mp4),
    ]
    res2 = subprocess.run(cmd_enc, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
    return res2.returncode == 0 and dst_mp4.is_file() and dst_mp4.stat().st_size > 1024


class ClipHandler(BaseHTTPRequestHandler):
    server_version = "InstantReplayShare/2.0"
    protocol_version = "HTTP/1.1"

    def end_headers(self):
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "SAMEORIGIN")
        self.send_header("Referrer-Policy", "strict-origin-when-cross-origin")
        super().end_headers()

    def _get_cookie_token(self, username: str) -> str:
        raw_cookie = self.headers.get("Cookie", "")
        if not raw_cookie:
            return ""
        c = cookies.SimpleCookie()
        try:
            c.load(raw_cookie)
        except Exception:
            return ""
        key = f"ir_token_{sanitize_username(username)}"
        if key in c:
            return c[key].value
        return ""

    def _is_owner_of(self, username: str, query_token: str = "") -> bool:
        user = sanitize_username(username)
        # Check header token, query token, or cookie token
        hdr_tok = self.headers.get("X-User-Token", "").strip()
        if hdr_tok and check_user_token_readonly(user, hdr_tok):
            return True
        if query_token and check_user_token_readonly(user, query_token):
            return True
        cookie_tok = self._get_cookie_token(user)
        if cookie_tok and check_user_token_readonly(user, cookie_tok):
            return True
        return False

    def _read_json_body(self) -> dict | None:
        cl = int(self.headers.get("Content-Length", "0"))
        if cl <= 0 or cl > 65536:
            return None
        raw = self.rfile.read(cl)
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return None

    def do_HEAD(self):
        self._is_head = True
        self.do_GET()

    def _write_body(self, data: bytes):
        if getattr(self, "_is_head", False):
            return
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass

    def _send_json(self, status: int, payload: dict):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self._write_body(body)

    def _send_html(self, status: int, html_str: str, extra_headers: dict[str, str] | None = None):
        body = html_str.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        self._write_body(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)

        if path == "/health":
            self._send_json(200, {"status": "ok"})
            return

        # Root or /gallery -> Public community gallery
        if path in ("/", "/gallery"):
            clips = load_all_clips(username_filter=None, include_private=False)
            cards_html = []
            for c in clips:
                cid = html.escape(c["id"])
                title = html.escape(c["title"])
                fdate = html.escape(c["formatted_date"])
                u = html.escape(c["username"])
                size_mb = c.get("size", 0) / (1024 * 1024)
                cards_html.append(f"""
                <div class="clip-card">
                  <a class="thumb-link" href="/v/{cid}">
                    <video muted loop playsinline preload="none" poster="/thumb/{cid}.jpg?v={c['mtime']}"
                           data-src="/stream/{cid}?v={c['mtime']}"
                           onmouseenter="this._ht=setTimeout(()=>{{if(!this.src)this.src=this.dataset.src;this.play().catch(()=>{{}});}},220);"
                           onmouseleave="clearTimeout(this._ht);this.pause();if(this.src){{this.removeAttribute('src');this.load();}}"></video>
                  </a>
                  <div class="meta">
                    <div class="top-row">
                      <a class="app-title" href="/v/{cid}">{title}</a>
                      <a class="user-pill" href="/u/{u}">@{u}</a>
                    </div>
                    <div class="sub">{fdate} &middot; {size_mb:.1f} MB</div>
                  </div>
                </div>""")
            empty_msg = '<p class="empty">No public clips yet. Owners can mark clips as Public from their gallery.</p>' if not cards_html else ""
            page = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Public Clip Gallery - InstantReplay</title>
<style>
  body {{ margin:0; background:#0d1117; color:#e6edf3; font-family:system-ui,-apple-system,sans-serif; padding:32px 24px; }}
  .container {{ max-width:1280px; margin:0 auto; }}
  header {{ display:flex; justify-content:space-between; align-items:baseline; margin-bottom:24px; border-bottom:1px solid #21262d; padding-bottom:16px; }}
  h1 {{ margin:0; font-size:24px; font-weight:600; }}
  .count {{ color:#8b949e; font-size:14px; }}
  .grid {{ display:grid; grid-template-columns:repeat(auto-fill, minmax(320px, 1fr)); gap:20px; }}
  .clip-card {{ background:#161b22; border:1px solid #30363d; border-radius:10px; overflow:hidden; display:flex; flex-direction:column; transition:border-color 0.15s; }}
  .clip-card:hover {{ border-color:#58a6ff; }}
  video {{ width:100%; aspect-ratio:16/9; background:#000; object-fit:cover; display:block; }}
  .meta {{ padding:12px 14px; }}
  .top-row {{ display:flex; justify-content:space-between; align-items:center; gap:8px; margin-bottom:4px; }}
  .app-title {{ font-weight:600; font-size:15px; color:#f0f6fc; text-decoration:none; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }}
  .app-title:hover {{ color:#58a6ff; }}
  .user-pill {{ font-size:12px; color:#58a6ff; background:#1f2937; padding:2px 8px; border-radius:999px; text-decoration:none; flex-shrink:0; }}
  .sub {{ color:#8b949e; font-size:13px; }}
  .empty {{ color:#8b949e; font-size:15px; }}
</style>
</head>
<body>
  <div class="container">
    <header>
      <h1>Public Clip Gallery</h1>
      <span class="count">{len(clips)} public clip{"s" if len(clips) != 1 else ""}</span>
    </header>
    {empty_msg}
    <div class="grid">{''.join(cards_html)}</div>
  </div>
</body>
</html>"""
            self._send_html(200, page)
            return

        # Per-user gallery: /u/<username>
        if path.startswith("/u/"):
            user = sanitize_username(path[3:].strip("/"))
            if not USER_RE.match(user):
                self.send_error(404)
                return

            url_token = qs.get("token", [""])[0].strip()
            if url_token:
                # Associate token with username if unclaimed or verify if already claimed,
                # then set HttpOnly cookie and redirect cleanly to /u/<username>
                if verify_or_register_user_token(user, url_token):
                    cookie_hdr = f"ir_token_{user}={url_token}; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax"
                    self.send_response(302)
                    self.send_header("Set-Cookie", cookie_hdr)
                    self.send_header("Location", f"/u/{user}")
                    self.end_headers()
                    return

            is_owner = self._is_owner_of(user)
            clips = load_all_clips(username_filter=user, include_private=is_owner)
            esc_user = html.escape(user)
            owner_badge = ""

            cards_html = []
            for c in clips:
                cid = html.escape(c["id"])
                title = html.escape(c["title"])
                fdate = html.escape(c["formatted_date"])
                size_mb = c.get("size", 0) / (1024 * 1024)
                vis_badge = ""
                edit_btn = ""
                if is_owner:
                    if c["public"]:
                        vis_badge = '<span class="vis-badge pub">Public</span>'
                    else:
                        vis_badge = '<span class="vis-badge priv">Private (Link Only)</span>'
                    edit_btn = f'<a class="edit-link" href="/v/{cid}#editor">Trim / Edit</a>'

                cards_html.append(f"""
                <div class="clip-card">
                  <a class="thumb-link" href="/v/{cid}">
                    <video muted loop playsinline preload="none" poster="/thumb/{cid}.jpg?v={c['mtime']}"
                           data-src="/stream/{cid}?v={c['mtime']}"
                           onmouseenter="this._ht=setTimeout(()=>{{if(!this.src)this.src=this.dataset.src;this.play().catch(()=>{{}});}},220);"
                           onmouseleave="clearTimeout(this._ht);this.pause();if(this.src){{this.removeAttribute('src');this.load();}}"></video>
                  </a>
                  <div class="meta">
                    <div class="top-row">
                      <a class="app-title" href="/v/{cid}">{title}</a>
                      {vis_badge}
                    </div>
                    <div class="bottom-row">
                      <span class="sub">{fdate} &middot; {size_mb:.1f} MB</span>
                      {edit_btn}
                    </div>
                  </div>
                </div>""")

            if not cards_html:
                empty_msg = (
                    '<p class="empty">No clips yet. Press Alt+F10 in-game to save and upload!</p>'
                    if is_owner else
                    '<p class="empty">This user has not published any public clips yet.</p>'
                )
            else:
                empty_msg = ""
            page = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>@{esc_user}'s Clips</title>
<style>
  body {{ margin:0; background:#0d1117; color:#e6edf3; font-family:system-ui,-apple-system,sans-serif; padding:32px 24px; }}
  .container {{ max-width:1280px; margin:0 auto; }}
  header {{ display:flex; justify-content:space-between; align-items:center; margin-bottom:24px; border-bottom:1px solid #21262d; padding-bottom:16px; flex-wrap:wrap; gap:12px; }}
  .title-grp {{ display:flex; align-items:center; gap:12px; }}
  h1 {{ margin:0; font-size:24px; font-weight:600; }}
  .owner-pill {{ background:rgba(46,160,67,0.18); border:1px solid #2ea043; color:#3fb950; font-size:12px; padding:4px 10px; border-radius:999px; font-weight:500; }}
  .nav-links {{ display:flex; align-items:center; gap:16px; font-size:14px; }}
  .nav-links a {{ color:#58a6ff; text-decoration:none; }}
  .nav-links a:hover {{ text-decoration:underline; }}
  .count {{ color:#8b949e; }}
  .grid {{ display:grid; grid-template-columns:repeat(auto-fill, minmax(330px, 1fr)); gap:20px; }}
  .clip-card {{ background:#161b22; border:1px solid #30363d; border-radius:10px; overflow:hidden; display:flex; flex-direction:column; transition:border-color 0.15s; }}
  .clip-card:hover {{ border-color:#58a6ff; }}
  video {{ width:100%; aspect-ratio:16/9; background:#000; object-fit:cover; display:block; }}
  .meta {{ padding:12px 14px; display:flex; flex-direction:column; gap:6px; }}
  .top-row {{ display:flex; justify-content:space-between; align-items:center; gap:8px; }}
  .bottom-row {{ display:flex; justify-content:space-between; align-items:center; gap:8px; }}
  .app-title {{ font-weight:600; font-size:15px; color:#f0f6fc; text-decoration:none; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }}
  .app-title:hover {{ color:#58a6ff; }}
  .vis-badge {{ font-size:11px; padding:2px 7px; border-radius:6px; font-weight:500; flex-shrink:0; }}
  .vis-badge.pub {{ background:rgba(56,139,253,0.18); color:#58a6ff; border:1px solid rgba(56,139,253,0.4); }}
  .vis-badge.priv {{ background:rgba(139,148,158,0.15); color:#8b949e; border:1px solid #30363d; }}
  .edit-link {{ font-size:12px; color:#58a6ff; text-decoration:none; background:#21262d; padding:3px 9px; border-radius:6px; border:1px solid #30363d; }}
  .edit-link:hover {{ border-color:#58a6ff; }}
  .sub {{ color:#8b949e; font-size:13px; }}
  .empty {{ color:#8b949e; font-size:15px; }}
</style>
</head>
<body>
  <div class="container">
    <header>
      <div class="title-grp">
        <h1>@{esc_user}</h1>
        {owner_badge}
      </div>
      <div class="nav-links">
        <a href="/gallery">Public Gallery</a>
        <span class="count">{len(clips)} clip{"s" if len(clips) != 1 else ""}</span>
      </div>
    </header>
    {empty_msg}
    <div class="grid">{''.join(cards_html)}</div>
  </div>
</body>
</html>"""
            self._send_html(200, page)
            return

        # Individual clip view + interactive owner trimmer/editor: /v/<id>
        if path.startswith("/v/"):
            clip_id = path[3:].strip("/")
            meta = load_clip_meta(clip_id)
            if not meta:
                self.send_error(404)
                return

            username = meta["username"]
            is_owner = self._is_owner_of(username, qs.get("token", [""])[0])

            esc_title = html.escape(meta["title"])
            esc_app = html.escape(meta["app_name"])
            esc_date = html.escape(meta["formatted_date"])
            esc_user = html.escape(username)
            og_title = html.escape(f"{meta['title']} \u2022 {meta['formatted_date']}")
            is_pub = meta["public"]
            mtime = meta["mtime"]

            raw_url = f"{PUBLIC_URL}/f/{clip_id}.mp4"
            thumb_url = f"{PUBLIC_URL}/thumb/{clip_id}.jpg"
            page_url = f"{PUBLIC_URL}/v/{clip_id}"
            og_desc = html.escape(f"Recorded in {meta['app_name']} by @{username}")

            vis_pill = (
                '<span class="vis-pill pub">Public (In Gallery)</span>'
                if is_pub else
                '<span class="vis-pill priv">Private (Accessible via Link)</span>'
            )

            editor_html = ""
            edit_toggle_btn = ""
            if is_owner:
                edit_toggle_btn = '<button type="button" class="badge btn-toggle-edit" id="btnToggleEditor">Trim / Edit</button>'
                pub_checked = "checked" if is_pub else ""
                priv_checked = "" if is_pub else "checked"
                editor_html = f"""
    <div id="editor" class="editor-panel" style="display:none;">
      <div class="editor-header">
        <h3>Trim &amp; Edit Clip</h3>
        <div class="quick-actions">
          <button type="button" class="btn btn-secondary" id="btnLoopPreview">Loop Selection: Off</button>
          <button type="button" class="btn btn-danger" id="btnDeleteClip">Delete Clip</button>
        </div>
      </div>

      <!-- Draggable Dual-Handle Timeline Bar -->
      <div class="timeline-wrap">
        <div class="timeline-track" id="timelineTrack">
          <div class="timeline-selection" id="timelineSelection"></div>
          <div class="timeline-playhead" id="timelinePlayhead"></div>
          <div class="timeline-handle left" id="handleStart" title="Drag Start Time"></div>
          <div class="timeline-handle right" id="handleEnd" title="Drag End Time"></div>
        </div>
        <div class="time-inputs">
          <label>Start (s):
            <input type="number" id="inputStart" step="0.1" min="0" value="0.0">
          </label>
          <span class="duration-readout" id="trimDurationLabel">Selected: -- s</span>
          <label>End (s):
            <input type="number" id="inputEnd" step="0.1" min="0.1" value="0.0">
          </label>
        </div>
      </div>

      <!-- Save / Visibility Options -->
      <div class="settings-row">
        <div class="field-group">
          <label for="inputTitle">Clip Title / Game Name</label>
          <input type="text" id="inputTitle" value="{esc_title}" maxlength="80">
        </div>
        <div class="field-group">
          <label>Visibility</label>
          <div class="radio-group">
            <label class="radio-opt">
              <input type="radio" name="visMode" value="private" {priv_checked}>
              Private (Link Only)
            </label>
            <label class="radio-opt">
              <input type="radio" name="visMode" value="public" {pub_checked}>
              Public (Show in Gallery)
            </label>
          </div>
        </div>
      </div>

      <div class="save-actions">
        <button type="button" class="btn btn-secondary" id="btnSaveMetaOnly">Save Title / Visibility Only</button>
        <button type="button" class="btn btn-primary" id="btnSaveCopy">Trim &amp; Save as New Copy</button>
        <button type="button" class="btn btn-warning" id="btnOverwrite">Trim &amp; Overwrite Original</button>
      </div>
      <div id="editorStatus" class="status-msg"></div>
    </div>

    <script>
    (() => {{
      const editorPanel = document.getElementById('editor');
      const btnToggleEditor = document.getElementById('btnToggleEditor');
      const video = document.getElementById('clipVideo');
      const track = document.getElementById('timelineTrack');
      const selBox = document.getElementById('timelineSelection');
      const playhead = document.getElementById('timelinePlayhead');
      const hStart = document.getElementById('handleStart');
      const hEnd = document.getElementById('handleEnd');
      const inStart = document.getElementById('inputStart');
      const inEnd = document.getElementById('inputEnd');
      const durLabel = document.getElementById('trimDurationLabel');
      const btnLoop = document.getElementById('btnLoopPreview');
      const inTitle = document.getElementById('inputTitle');
      const statusEl = document.getElementById('editorStatus');

      let duration = 0;
      let startSec = 0;
      let endSec = 0;
      let loopPreview = false;
      let dragging = null;

      function toggleEditor(forceOpen) {{
        const open = (typeof forceOpen === 'boolean') ? forceOpen : (editorPanel.style.display === 'none');
        editorPanel.style.display = open ? 'flex' : 'none';
        if (btnToggleEditor) btnToggleEditor.textContent = open ? 'Close Editor' : 'Trim / Edit';
        if (open) syncUI();
      }}
      if (btnToggleEditor) {{
        btnToggleEditor.addEventListener('click', () => toggleEditor());
      }}
      if (window.location.hash === '#editor') {{
        toggleEditor(true);
      }}

      function clamp(v, lo, hi) {{ return Math.max(lo, Math.min(hi, v)); }}

      function syncUI() {{
        if (duration <= 0) return;
        const leftPct = (startSec / duration) * 100;
        const rightPct = (endSec / duration) * 100;
        selBox.style.left = leftPct + '%';
        selBox.style.width = Math.max(0, rightPct - leftPct) + '%';
        hStart.style.left = leftPct + '%';
        hEnd.style.left = rightPct + '%';
        inStart.value = startSec.toFixed(1);
        inEnd.value = endSec.toFixed(1);
        durLabel.textContent = 'Selected: ' + Math.max(0, endSec - startSec).toFixed(1) + 's / ' + duration.toFixed(1) + 's';
      }}

      video.addEventListener('loadedmetadata', () => {{
        duration = video.duration || 10;
        startSec = 0;
        endSec = duration;
        inStart.max = duration.toFixed(1);
        inEnd.max = duration.toFixed(1);
        syncUI();
      }});

      video.addEventListener('timeupdate', () => {{
        if (duration > 0) {{
          const pct = clamp((video.currentTime / duration) * 100, 0, 100);
          playhead.style.left = pct + '%';
        }}
        if (loopPreview && video.currentTime >= endSec) {{
          video.currentTime = startSec;
          video.play();
        }}
      }});

      function clientXToSec(clientX) {{
        const rect = track.getBoundingClientRect();
        const ratio = clamp((clientX - rect.left) / rect.width, 0, 1);
        return ratio * duration;
      }}

      hStart.addEventListener('pointerdown', (e) => {{ dragging = 'start'; hStart.setPointerCapture(e.pointerId); e.preventDefault(); }});
      hEnd.addEventListener('pointerdown', (e) => {{ dragging = 'end'; hEnd.setPointerCapture(e.pointerId); e.preventDefault(); }});

      window.addEventListener('pointermove', (e) => {{
        if (!dragging || duration <= 0) return;
        const t = clientXToSec(e.clientX);
        if (dragging === 'start') {{
          startSec = clamp(t, 0, Math.max(0, endSec - 0.2));
          video.currentTime = startSec;
        }} else if (dragging === 'end') {{
          endSec = clamp(t, Math.min(duration, startSec + 0.2), duration);
          video.currentTime = endSec;
        }}
        syncUI();
      }});

      window.addEventListener('pointerup', () => {{ dragging = null; }});

      inStart.addEventListener('change', () => {{
        startSec = clamp(parseFloat(inStart.value) || 0, 0, Math.max(0, endSec - 0.2));
        video.currentTime = startSec;
        syncUI();
      }});

      inEnd.addEventListener('change', () => {{
        endSec = clamp(parseFloat(inEnd.value) || duration, Math.min(duration, startSec + 0.2), duration);
        video.currentTime = endSec;
        syncUI();
      }});

      btnLoop.addEventListener('click', () => {{
        loopPreview = !loopPreview;
        btnLoop.textContent = 'Loop Selection: ' + (loopPreview ? 'On' : 'Off');
        if (loopPreview) {{
          video.currentTime = startSec;
          video.play();
        }}
      }});

      function getSelectedPublic() {{
        const r = document.querySelector('input[name="visMode"]:checked');
        return r && r.value === 'public';
      }}

      async function submitEdit(mode) {{
        statusEl.textContent = mode === 'meta' ? 'Saving settings...' : 'Rendering trimmed video on server...';
        statusEl.className = 'status-msg info';
        try {{
          const endpoint = mode === 'meta' ? '/api/clips/{clip_id}/meta' : '/api/clips/{clip_id}/trim';
          const resp = await fetch(endpoint, {{
            method: 'POST',
            headers: {{ 'Content-Type': 'application/json' }},
            body: JSON.stringify({{
              start_sec: startSec,
              end_sec: endSec,
              mode: mode,
              title: inTitle.value.trim(),
              public: getSelectedPublic()
            }})
          }});
          const data = await resp.json();
          if (!resp.ok) throw new Error(data.error || 'Request failed');
          if (mode === 'copy' && data.id) {{
            statusEl.textContent = 'Saved new copy! Redirecting...';
            window.location.href = '/v/' + data.id;
          }} else {{
            statusEl.textContent = 'Saved! Reloading...';
            window.location.reload();
          }}
        }} catch (err) {{
          statusEl.textContent = 'Error: ' + err.message;
          statusEl.className = 'status-msg err';
        }}
      }}

      document.getElementById('btnSaveMetaOnly').addEventListener('click', () => submitEdit('meta'));
      document.getElementById('btnSaveCopy').addEventListener('click', () => submitEdit('copy'));
      document.getElementById('btnOverwrite').addEventListener('click', () => {{
        if (confirm('Overwrite original video clip with this trimmed range?')) {{
          submitEdit('overwrite');
        }}
      }});

      document.getElementById('btnDeleteClip').addEventListener('click', async () => {{
        if (!confirm('Permanently delete this clip?')) return;
        const resp = await fetch('/api/clips/{clip_id}', {{ method: 'DELETE' }});
        if (resp.ok) window.location.href = '/u/{esc_user}';
      }});
    }})();
    </script>"""

            page = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{og_title}</title>
<meta property="og:title" content="{og_title}">
<meta property="og:description" content="{og_desc}">
<meta property="og:site_name" content="@{esc_user} on InstantReplay">
<meta property="og:type" content="video.other">
<meta property="og:url" content="{page_url}">
<meta property="og:image" content="{thumb_url}">
<meta property="og:video" content="{raw_url}">
<meta property="og:video:url" content="{raw_url}">
<meta property="og:video:secure_url" content="{raw_url}">
<meta property="og:video:type" content="video/mp4">
<meta property="og:video:width" content="1920">
<meta property="og:video:height" content="1080">
<meta name="twitter:card" content="player">
<meta name="twitter:title" content="{og_title}">
<meta name="twitter:description" content="{og_desc}">
<meta name="twitter:image" content="{thumb_url}">
<meta name="twitter:player:width" content="1920">
<meta name="twitter:player:height" content="1080">
<meta name="twitter:player:stream" content="{raw_url}">
<meta name="twitter:player:stream:content_type" content="video/mp4">
<meta name="theme-color" content="#58a6ff">
<style>
  body {{ margin:0; background:#0d1117; color:#e6edf3; font-family:system-ui,-apple-system,sans-serif; display:flex; flex-direction:column; align-items:center; justify-content:center; min-height:100vh; padding:20px; box-sizing:border-box; }}
  .card {{ max-width:95vw; width:1120px; background:#161b22; border:1px solid #30363d; border-radius:12px; padding:18px; box-shadow:0 8px 28px rgba(0,0,0,0.45); }}
  video {{ width:100%; max-height:74vh; border-radius:8px; background:#000; display:block; }}
  .bar {{ display:flex; justify-content:space-between; align-items:center; margin-top:14px; gap:12px; flex-wrap:wrap; }}
  .info {{ display:flex; flex-direction:column; gap:4px; }}
  .title-line {{ display:flex; align-items:center; gap:10px; flex-wrap:wrap; }}
  .app-name {{ font-size:18px; font-weight:600; color:#f0f6fc; }}
  .sub-app {{ font-size:12px; color:#8b949e; }}
  .date-line {{ font-size:13px; color:#8b949e; }}
  .vis-pill {{ font-size:11px; padding:2px 8px; border-radius:999px; font-weight:500; }}
  .vis-pill.pub {{ background:rgba(56,139,253,0.18); color:#58a6ff; border:1px solid rgba(56,139,253,0.4); }}
  .vis-pill.priv {{ background:rgba(139,148,158,0.15); color:#8b949e; border:1px solid #30363d; }}
  .actions {{ display:flex; align-items:center; gap:12px; font-size:14px; }}
  .badge {{ background:#21262d; border:1px solid #30363d; padding:6px 12px; border-radius:6px; color:#c9d1d9; text-decoration:none; font-family:inherit; font-size:14px; cursor:pointer; }}
  .badge:hover {{ border-color:#58a6ff; color:#58a6ff; }}
  a.dl {{ color:#58a6ff; text-decoration:none; font-weight:500; }}
  a.dl:hover {{ text-decoration:underline; }}

  /* Owner Trimmer & Studio */
  .editor-panel {{ margin-top:20px; padding-top:18px; border-top:1px solid #30363d; display:flex; flex-direction:column; gap:16px; }}
  .editor-header {{ display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; }}
  .editor-header h3 {{ margin:0; font-size:16px; }}
  .sub-hint {{ font-weight:400; color:#8b949e; font-size:13px; }}
  .quick-actions {{ display:flex; gap:8px; }}
  .timeline-wrap {{ display:flex; flex-direction:column; gap:10px; background:#0d1117; padding:14px; border-radius:8px; border:1px solid #21262d; }}
  .timeline-track {{ position:relative; height:28px; background:#21262d; border-radius:6px; user-select:none; touch-action:none; cursor:pointer; }}
  .timeline-selection {{ position:absolute; top:0; bottom:0; background:rgba(56,139,253,0.32); border-top:2px solid #58a6ff; border-bottom:2px solid #58a6ff; pointer-events:none; }}
  .timeline-playhead {{ position:absolute; top:-3px; bottom:-3px; width:2px; background:#f0f6fc; pointer-events:none; }}
  .timeline-handle {{ position:absolute; top:-4px; bottom:-4px; width:14px; margin-left:-7px; background:#58a6ff; border:2px solid #f0f6fc; border-radius:4px; cursor:ew-resize; z-index:3; }}
  .timeline-handle:hover {{ background:#79c0ff; }}
  .time-inputs {{ display:flex; justify-content:space-between; align-items:center; font-size:13px; color:#c9d1d9; flex-wrap:wrap; gap:8px; }}
  .time-inputs input {{ width:80px; background:#161b22; border:1px solid #30363d; color:#f0f6fc; border-radius:5px; padding:4px 8px; font-size:13px; margin-left:4px; }}
  .duration-readout {{ color:#58a6ff; font-weight:500; }}
  .settings-row {{ display:grid; grid-template-columns:1fr 1fr; gap:16px; }}
  @media (max-width:700px) {{ .settings-row {{ grid-template-columns:1fr; }} }}
  .field-group {{ display:flex; flex-direction:column; gap:6px; font-size:13px; }}
  .field-group label {{ color:#8b949e; font-weight:500; }}
  .field-group input[type="text"] {{ background:#0d1117; border:1px solid #30363d; color:#f0f6fc; border-radius:6px; padding:8px 10px; font-size:14px; }}
  .radio-group {{ display:flex; gap:16px; align-items:center; padding-top:4px; flex-wrap:wrap; }}
  .radio-opt {{ display:flex; align-items:center; gap:6px; color:#e6edf3 !important; cursor:pointer; }}
  .save-actions {{ display:flex; gap:10px; flex-wrap:wrap; }}
  .btn {{ border:1px solid #30363d; border-radius:6px; padding:8px 14px; font-size:13px; font-weight:600; cursor:pointer; color:#f0f6fc; background:#21262d; }}
  .btn:hover {{ filter:brightness(1.12); }}
  .btn-primary {{ background:#238636; border-color:#2ea043; }}
  .btn-warning {{ background:#9e6a03; border-color:#bb8009; }}
  .btn-danger {{ background:#da3633; border-color:#f85149; }}
  .status-msg {{ font-size:13px; min-height:18px; }}
  .status-msg.info {{ color:#58a6ff; }}
  .status-msg.err {{ color:#f85149; }}
</style>
</head>
<body>
  <div class="card">
    <video id="clipVideo" controls autoplay playsinline preload="auto" poster="/thumb/{clip_id}.jpg?v={mtime}" src="/stream/{clip_id}?v={mtime}"></video>
    <div class="bar">
      <div class="info">
        <div class="title-line">
          <span class="app-name">{esc_title}</span>
          {vis_pill}
        </div>
        <span class="date-line">{esc_date} &middot; Recorded in {esc_app}</span>
      </div>
      <div class="actions">
        {edit_toggle_btn}
        <a class="badge" href="/u/{esc_user}">@{esc_user}'s Gallery</a>
        <a class="dl" href="/f/{clip_id}.mp4?v={mtime}" download>Download</a>
      </div>
    </div>
    {editor_html}
  </div>
</body>
</html>"""
            self._send_html(200, page)
            return

        if path.startswith("/thumb/") and path.endswith(".jpg"):
            clip_id = path[len("/thumb/"):-4]
            if not ID_RE.match(clip_id):
                self.send_error(404)
                return
            mp4_path = DATA_DIR / f"{clip_id}.mp4"
            if not mp4_path.is_file():
                self.send_error(404)
                return
            jpg_path = ensure_thumbnail(clip_id, mp4_path)
            if not jpg_path or not jpg_path.is_file():
                self.send_error(404)
                return
            data = jpg_path.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Cache-Control", "public, max-age=86400")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self._write_body(data)
            return

        if path.startswith("/stream/") or (path.startswith("/f/") and path.endswith(".mp4")):
            if path.startswith("/stream/"):
                clip_id = path[len("/stream/"):].strip("/")
            else:
                clip_id = path[3:-4]
            if not ID_RE.match(clip_id):
                self.send_error(404)
                return
            mp4_path = DATA_DIR / f"{clip_id}.mp4"
            if not mp4_path.is_file():
                self.send_error(404)
                return
            file_size = mp4_path.stat().st_size
            range_hdr = self.headers.get("Range")
            start, end = 0, file_size - 1
            status = 200

            if range_hdr and range_hdr.startswith("bytes="):
                try:
                    r_part = range_hdr[6:].split(",")[0].strip()
                    s_str, e_str = r_part.split("-")
                    if s_str:
                        start = int(s_str)
                        end = int(e_str) if e_str else file_size - 1
                    elif e_str:
                        start = max(0, file_size - int(e_str))
                    end = min(end, file_size - 1)
                    if 0 <= start <= end:
                        status = 206
                except ValueError:
                    pass

            length = end - start + 1
            self.send_response(status)
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Accept-Ranges", "bytes")
            # Prevent Cloudflare from stripping Range headers to pre-cache 100MB+ MP4 files
            self.send_header("Cache-Control", "private, no-store, no-transform")
            self.send_header("CDN-Cache-Control", "no-store")
            self.send_header("Cloudflare-CDN-Cache-Control", "no-store")
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
            self.send_header("Content-Length", str(length))
            self.end_headers()

            if getattr(self, "_is_head", False):
                return

            try:
                with open(mp4_path, "rb") as f:
                    f.seek(start)
                    remaining = length
                    while remaining > 0:
                        chunk = f.read(min(256 * 1024, remaining))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        remaining -= len(chunk)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass
            return

        self.send_error(404)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        # Metadata-only update (title / public-private toggle)
        if path.startswith("/api/clips/") and path.endswith("/meta"):
            clip_id = path[len("/api/clips/"):-len("/meta")].strip("/")
            meta = load_clip_meta(clip_id)
            if not meta:
                self._send_json(404, {"error": "clip not found"})
                return
            if not self._is_owner_of(meta["username"]):
                self._send_json(403, {"error": "not clip owner"})
                return
            body = self._read_json_body()
            if not body:
                self._send_json(400, {"error": "invalid json"})
                return
            if "title" in body and str(body["title"]).strip():
                meta["title"] = sanitize_app_name(str(body["title"]))
            if "public" in body:
                meta["public"] = bool(body["public"])
            save_clip_meta(meta)
            self._send_json(200, meta)
            return

        # Backend video trim + save as copy or overwrite
        if path.startswith("/api/clips/") and path.endswith("/trim"):
            clip_id = path[len("/api/clips/"):-len("/trim")].strip("/")
            meta = load_clip_meta(clip_id)
            if not meta:
                self._send_json(404, {"error": "clip not found"})
                return
            if not self._is_owner_of(meta["username"]):
                self._send_json(403, {"error": "not clip owner"})
                return
            body = self._read_json_body()
            if not body:
                self._send_json(400, {"error": "invalid json"})
                return

            start_sec = max(0.0, float(body.get("start_sec", 0.0)))
            end_sec = float(body.get("end_sec", 0.0))
            if end_sec <= start_sec + 0.15:
                self._send_json(400, {"error": "end_sec must be greater than start_sec"})
                return

            mode = str(body.get("mode", "copy")).lower()
            new_title = sanitize_app_name(str(body.get("title", "")).strip() or meta["title"])
            is_public = bool(body.get("public", meta.get("public", False)))

            src_mp4 = DATA_DIR / f"{clip_id}.mp4"
            tmp_out = DATA_DIR / f".trim_{secrets.token_hex(4)}.mp4"

            ok = run_ffmpeg_trim(src_mp4, tmp_out, start_sec, end_sec)
            if not ok:
                tmp_out.unlink(missing_ok=True)
                self._send_json(500, {"error": "ffmpeg trim failed"})
                return

            if mode == "overwrite":
                shutil.move(str(tmp_out), str(src_mp4))
                meta["title"] = new_title
                meta["public"] = is_public
                meta["size"] = src_mp4.stat().st_size
                save_clip_meta(meta)
                self._send_json(200, meta)
                return
            else:
                new_id = secrets.token_urlsafe(6)[:8]
                dst_mp4 = DATA_DIR / f"{new_id}.mp4"
                shutil.move(str(tmp_out), str(dst_mp4))
                now_ts = int(time.time())
                new_meta = {
                    "id": new_id,
                    "username": meta["username"],
                    "app_name": meta["app_name"],
                    "title": new_title,
                    "public": is_public,
                    "size": dst_mp4.stat().st_size,
                    "created_at": now_ts,
                    "formatted_date": format_timestamp(now_ts),
                    "url": f"{PUBLIC_URL}/v/{new_id}",
                    "gallery": f"{PUBLIC_URL}/u/{meta['username']}",
                    "raw": f"{PUBLIC_URL}/f/{new_id}.mp4",
                }
                save_clip_meta(new_meta)
                enforce_storage_quota()
                self._send_json(201, new_meta)
                return

        if path != "/api/upload":
            self.send_error(404)
            return

        username = sanitize_username(self.headers.get("X-Username", "default"))
        user_token = self.headers.get("X-User-Token", "").strip()

        # Authenticate via per-user token (associating on first use) OR global UPLOAD_TOKEN
        authed = False
        if user_token and verify_or_register_user_token(username, user_token):
            authed = True
        elif UPLOAD_TOKEN:
            auth_hdr = self.headers.get("Authorization", "")
            if secrets.compare_digest(auth_hdr, f"Bearer {UPLOAD_TOKEN}"):
                authed = True

        if not authed:
            self._send_json(401, {"error": "unauthorized (username already claimed by another user_token)"})
            return

        cl_str = self.headers.get("Content-Length")
        if not cl_str:
            self._send_json(411, {"error": "Content-Length required"})
            return

        total_bytes = int(cl_str)
        if total_bytes <= 0 or total_bytes > MAX_UPLOAD_BYTES:
            self._send_json(413, {"error": "payload too large"})
            return

        # Support chunked uploads to bypass Cloudflare's 100MB per-request limit
        upload_id_hdr = self.headers.get("X-Upload-Id", "").strip()
        chunk_idx_hdr = self.headers.get("X-Chunk-Index", "").strip()
        total_chunks_hdr = self.headers.get("X-Total-Chunks", "").strip()

        if upload_id_hdr and chunk_idx_hdr.isdigit() and total_chunks_hdr.isdigit():
            if not ID_RE.match(upload_id_hdr):
                self._send_json(400, {"error": "invalid X-Upload-Id"})
                return
            chunk_idx = int(chunk_idx_hdr)
            total_chunks = int(total_chunks_hdr)
            if total_chunks < 1 or total_chunks > 200 or chunk_idx < 0 or chunk_idx >= total_chunks:
                self._send_json(400, {"error": "invalid chunk bounds"})
                return

            tmp_path = DATA_DIR / f".chunk_{upload_id_hdr}.tmp"
            mode = "wb" if chunk_idx == 0 else "ab"
            if chunk_idx > 0 and not tmp_path.is_file():
                self._send_json(400, {"error": "missing previous chunks"})
                return

            remaining = total_bytes
            with open(tmp_path, mode) as f:
                while remaining > 0:
                    chunk = self.rfile.read(min(256 * 1024, remaining))
                    if not chunk:
                        break
                    f.write(chunk)
                    remaining -= len(chunk)

            if remaining != 0:
                tmp_path.unlink(missing_ok=True)
                self._send_json(400, {"error": "incomplete chunk upload"})
                return

            if chunk_idx < total_chunks - 1:
                self._send_json(200, {"status": "chunk_ok", "chunk": chunk_idx})
                return

            # Final chunk received -> finalize clip
            clip_id = secrets.token_urlsafe(6)[:8]
            final_path = DATA_DIR / f"{clip_id}.mp4"
        else:
            clip_id = secrets.token_urlsafe(6)[:8]
            tmp_path = DATA_DIR / f".{clip_id}.tmp"
            final_path = DATA_DIR / f"{clip_id}.mp4"

            remaining = total_bytes
            with open(tmp_path, "wb") as f:
                while remaining > 0:
                    chunk = self.rfile.read(min(256 * 1024, remaining))
                    if not chunk:
                        break
                    f.write(chunk)
                    remaining -= len(chunk)

            if remaining != 0:
                tmp_path.unlink(missing_ok=True)
                self._send_json(400, {"error": "incomplete upload"})
                return

        shutil.move(str(tmp_path), str(final_path))
        remux_faststart_interleaved(final_path)
        final_size = final_path.stat().st_size

        app_name = sanitize_app_name(self.headers.get("X-App-Name", "Desktop Capture"))
        now_ts = int(time.time())

        meta = {
            "id": clip_id,
            "username": username,
            "app_name": app_name,
            "title": app_name,
            "public": False,  # Default Private (accessible by direct link, hidden from public gallery)
            "size": final_size,
            "created_at": now_ts,
            "formatted_date": format_timestamp(now_ts),
            "url": f"{PUBLIC_URL}/v/{clip_id}",
            "gallery": f"{PUBLIC_URL}/u/{username}",
            "raw": f"{PUBLIC_URL}/f/{clip_id}.mp4",
        }
        save_clip_meta(meta)
        threading.Thread(target=ensure_thumbnail, args=(clip_id, final_path), daemon=True).start()
        enforce_storage_quota()
        self._send_json(201, meta)

    def do_DELETE(self):
        parsed = urlparse(self.path)
        if not parsed.path.startswith("/api/clips/"):
            self.send_error(404)
            return
        clip_id = parsed.path[len("/api/clips/"):].strip("/")
        meta = load_clip_meta(clip_id)
        if not meta:
            self.send_error(404)
            return
        if not self._is_owner_of(meta["username"]):
            self._send_json(403, {"error": "not clip owner"})
            return
        (DATA_DIR / f"{clip_id}.mp4").unlink(missing_ok=True)
        (DATA_DIR / f"{clip_id}.jpg").unlink(missing_ok=True)
        (DATA_DIR / f"{clip_id}.json").unlink(missing_ok=True)
        self._send_json(200, {"deleted": clip_id})


if __name__ == "__main__":
    print(f"InstantReplay Share Server v2.0 listening on http://0.0.0.0:{PORT}")
    print(f"  Storage directory : {DATA_DIR}")
    print(f"  Public base URL   : {PUBLIC_URL}")
    httpd = ThreadingHTTPServer(("0.0.0.0", PORT), ClipHandler)
    httpd.serve_forever()
