#!/usr/bin/env python3
"""
Lightweight self-hosted clip share server (zero third-party dependencies).
Stores clips in DATA_DIR, displays recorded application +
formatted date/time (never raw filenames), and provides per-user galleries at /u/<username>.
"""
from datetime import datetime
import html
import json
import os
import re
import secrets
import shutil
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


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
UPLOAD_TOKEN = os.environ.get("UPLOAD_TOKEN", "change-me-in-env")
DATA_DIR = Path(_default_data_dir()).resolve()
MAX_UPLOAD_BYTES = int(os.environ.get("MAX_UPLOAD_MB", "2048")) * 1024 * 1024
MAX_STORAGE_BYTES = int(os.environ.get("MAX_STORAGE_GB", "50")) * 1024 * 1024 * 1024

ID_RE = re.compile(r"^[A-Za-z0-9_-]{4,32}$")
USER_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
DATA_DIR.mkdir(parents=True, exist_ok=True)


def format_timestamp(ts: int) -> str:
    dt = datetime.fromtimestamp(ts)
    hour12 = dt.hour % 12 or 12
    ampm = "AM" if dt.hour < 12 else "PM"
    return f"{dt.strftime('%b')} {dt.day}, {dt.year} \u2022 {hour12}:{dt.strftime('%M')} {ampm}"


def sanitize_username(raw: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_-]", "", (raw or "").strip())[:32]
    return cleaned.lower() if cleaned else "default"


def sanitize_app_name(raw: str) -> str:
    cleaned = re.sub(r"[\r\n\t]+", " ", (raw or "").strip())[:60]
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


def load_all_clips(username_filter: str | None = None) -> list[dict]:
    clips = []
    for meta_file in DATA_DIR.glob("*.json"):
        try:
            meta = json.loads(meta_file.read_text("utf-8"))
            if not (DATA_DIR / f"{meta.get('id', '')}.mp4").is_file():
                continue
            user = sanitize_username(meta.get("username", "default"))
            meta["username"] = user
            meta["app_name"] = sanitize_app_name(meta.get("app_name", "Desktop Capture"))
            meta["formatted_date"] = format_timestamp(int(meta.get("created_at", 0)))
            if username_filter and user != username_filter.lower():
                continue
            clips.append(meta)
        except Exception:
            pass
    clips.sort(key=lambda c: c.get("created_at", 0), reverse=True)
    return clips


class ClipHandler(BaseHTTPRequestHandler):
    server_version = "InstantReplayShare/1.1"

    def end_headers(self):
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "SAMEORIGIN")
        self.send_header("Referrer-Policy", "strict-origin-when-cross-origin")
        super().end_headers()

    def _check_auth(self) -> bool:
        auth = self.headers.get("Authorization", "")
        return secrets.compare_digest(auth, f"Bearer {UPLOAD_TOKEN}")

    def _send_json(self, status: int, payload: dict):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, status: int, html_str: str):
        body = html_str.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)

        if path == "/health":
            self._send_json(200, {"status": "ok"})
            return

        if path == "/api/clips":
            if not self._check_auth():
                self._send_json(401, {"error": "unauthorized"})
                return
            user_filter = qs.get("user", [None])[0]
            self._send_json(200, {"clips": load_all_clips(user_filter)})
            return

        # Per-user gallery: /u/<username>
        if path.startswith("/u/"):
            user = path[3:].strip("/").lower()
            if not USER_RE.match(user):
                self.send_error(404)
                return
            clips = load_all_clips(user)
            esc_user = html.escape(user)
            cards_html = []
            for c in clips:
                cid = html.escape(c["id"])
                app_title = html.escape(c["app_name"])
                fdate = html.escape(c["formatted_date"])
                size_mb = c.get("size", 0) / (1024 * 1024)
                cards_html.append(f"""
                <a class="clip-card" href="/v/{cid}">
                  <video muted loop playsinline preload="metadata" src="/f/{cid}.mp4#t=0.5"
                         onmouseenter="this.play()" onmouseleave="this.pause();this.currentTime=0.5;"></video>
                  <div class="meta">
                    <div class="app-title">{app_title}</div>
                    <div class="sub">{fdate} &middot; {size_mb:.1f} MB</div>
                  </div>
                </a>""")

            empty_msg = '<p class="empty">No clips uploaded yet.</p>' if not cards_html else ""
            page = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc_user}'s Clips</title>
<style>
  body {{ margin:0; background:#0d1117; color:#e6edf3; font-family:system-ui,-apple-system,sans-serif; padding:32px 24px; }}
  .container {{ max-width:1280px; margin:0 auto; }}
  header {{ display:flex; justify-content:space-between; align-items:baseline; margin-bottom:24px; border-bottom:1px solid #21262d; padding-bottom:16px; }}
  h1 {{ margin:0; font-size:24px; font-weight:600; }}
  .count {{ color:#8b949e; font-size:14px; }}
  .grid {{ display:grid; grid-template-columns:repeat(auto-fill, minmax(320px, 1fr)); gap:20px; }}
  .clip-card {{ background:#161b22; border:1px solid #30363d; border-radius:10px; overflow:hidden; text-decoration:none; color:inherit; transition:transform 0.15s, border-color 0.15s; display:flex; flex-direction:column; }}
  .clip-card:hover {{ transform:translateY(-2px); border-color:#58a6ff; }}
  video {{ width:100%; aspect-ratio:16/9; background:#000; object-fit:cover; display:block; }}
  .meta {{ padding:12px 14px; }}
  .app-title {{ font-weight:600; font-size:15px; color:#f0f6fc; margin-bottom:4px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }}
  .sub {{ color:#8b949e; font-size:13px; }}
  .empty {{ color:#8b949e; font-size:15px; }}
</style>
</head>
<body>
  <div class="container">
    <header>
      <h1>@{esc_user}</h1>
      <span class="count">{len(clips)} clip{"s" if len(clips) != 1 else ""}</span>
    </header>
    {empty_msg}
    <div class="grid">{''.join(cards_html)}</div>
  </div>
</body>
</html>"""
            self._send_html(200, page)
            return

        # Individual clip view: /v/<id>
        if path.startswith("/v/"):
            clip_id = path[3:].strip("/")
            if not ID_RE.match(clip_id):
                self.send_error(404)
                return
            mp4_path = DATA_DIR / f"{clip_id}.mp4"
            meta_path = DATA_DIR / f"{clip_id}.json"
            if not mp4_path.is_file():
                self.send_error(404)
                return

            created_ts = int(mp4_path.stat().st_mtime)
            app_name = "Desktop Capture"
            username = "default"
            if meta_path.is_file():
                try:
                    meta = json.loads(meta_path.read_text("utf-8"))
                    created_ts = int(meta.get("created_at", created_ts))
                    app_name = sanitize_app_name(meta.get("app_name", app_name))
                    username = sanitize_username(meta.get("username", username))
                except Exception:
                    pass

            nice_date = format_timestamp(created_ts)
            esc_app = html.escape(app_name)
            esc_date = html.escape(nice_date)
            esc_user = html.escape(username)
            og_title = html.escape(f"{app_name} \u2022 {nice_date}")

            raw_url = f"{PUBLIC_URL}/f/{clip_id}.mp4"
            page_url = f"{PUBLIC_URL}/v/{clip_id}"
            page = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{og_title}</title>
<meta property="og:title" content="{og_title}">
<meta property="og:site_name" content="@{esc_user} on InstantReplay">
<meta property="og:type" content="video.other">
<meta property="og:url" content="{page_url}">
<meta property="og:video" content="{raw_url}">
<meta property="og:video:secure_url" content="{raw_url}">
<meta property="og:video:type" content="video/mp4">
<meta name="twitter:card" content="player">
<style>
  body {{ margin:0; background:#0d1117; color:#e6edf3; font-family:system-ui,-apple-system,sans-serif; display:flex; flex-direction:column; align-items:center; justify-content:center; min-height:100vh; padding:16px; box-sizing:border-box; }}
  .card {{ max-width:94vw; width:1120px; background:#161b22; border:1px solid #30363d; border-radius:12px; padding:18px; box-shadow:0 8px 28px rgba(0,0,0,0.45); }}
  video {{ width:100%; max-height:78vh; border-radius:8px; background:#000; display:block; }}
  .bar {{ display:flex; justify-content:space-between; align-items:center; margin-top:14px; gap:12px; flex-wrap:wrap; }}
  .info {{ display:flex; flex-direction:column; gap:4px; }}
  .app-name {{ font-size:18px; font-weight:600; color:#f0f6fc; }}
  .date-line {{ font-size:13px; color:#8b949e; }}
  .actions {{ display:flex; align-items:center; gap:14px; font-size:14px; }}
  .badge {{ background:#21262d; border:1px solid #30363d; padding:5px 10px; border-radius:6px; color:#c9d1d9; text-decoration:none; }}
  .badge:hover {{ border-color:#58a6ff; color:#58a6ff; }}
  a.dl {{ color:#58a6ff; text-decoration:none; font-weight:500; }}
  a.dl:hover {{ text-decoration:underline; }}
</style>
</head>
<body>
  <div class="card">
    <video controls autoplay playsinline src="/f/{clip_id}.mp4"></video>
    <div class="bar">
      <div class="info">
        <span class="app-name">{esc_app}</span>
        <span class="date-line">{esc_date}</span>
      </div>
      <div class="actions">
        <a class="badge" href="/u/{esc_user}">@{esc_user}'s Gallery</a>
        <a class="dl" href="/f/{clip_id}.mp4" download>Download</a>
      </div>
    </div>
  </div>
</body>
</html>"""
            self._send_html(200, page)
            return

        if path.startswith("/f/") and path.endswith(".mp4"):
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
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
            self.send_header("Content-Length", str(length))
            self.end_headers()

            with open(mp4_path, "rb") as f:
                f.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = f.read(min(256 * 1024, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)
            return

        self.send_error(404)

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path != "/api/upload":
            self.send_error(404)
            return

        if not self._check_auth():
            self._send_json(401, {"error": "unauthorized"})
            return

        cl_str = self.headers.get("Content-Length")
        if not cl_str:
            self._send_json(411, {"error": "Content-Length required"})
            return

        total_bytes = int(cl_str)
        if total_bytes <= 0 or total_bytes > MAX_UPLOAD_BYTES:
            self._send_json(413, {"error": "payload too large"})
            return

        clip_id = secrets.token_urlsafe(6)[:8]
        tmp_path = DATA_DIR / f".{clip_id}.tmp"
        final_path = DATA_DIR / f"{clip_id}.mp4"
        meta_path = DATA_DIR / f"{clip_id}.json"

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
        username = sanitize_username(self.headers.get("X-Username", "default"))
        app_name = sanitize_app_name(self.headers.get("X-App-Name", "Desktop Capture"))
        now_ts = int(time.time())

        meta = {
            "id": clip_id,
            "username": username,
            "app_name": app_name,
            "size": total_bytes,
            "created_at": now_ts,
            "formatted_date": format_timestamp(now_ts),
            "url": f"{PUBLIC_URL}/v/{clip_id}",
            "gallery": f"{PUBLIC_URL}/u/{username}",
            "raw": f"{PUBLIC_URL}/f/{clip_id}.mp4",
        }
        meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")
        enforce_storage_quota()
        self._send_json(201, meta)

    def do_DELETE(self):
        parsed = urlparse(self.path)
        if not parsed.path.startswith("/api/clips/"):
            self.send_error(404)
            return
        if not self._check_auth():
            self._send_json(401, {"error": "unauthorized"})
            return
        clip_id = parsed.path[len("/api/clips/"):].strip("/")
        if not ID_RE.match(clip_id):
            self.send_error(404)
            return
        (DATA_DIR / f"{clip_id}.mp4").unlink(missing_ok=True)
        (DATA_DIR / f"{clip_id}.json").unlink(missing_ok=True)
        self._send_json(200, {"deleted": clip_id})


if __name__ == "__main__":
    print(f"InstantReplay Share Server listening on http://0.0.0.0:{PORT}")
    print(f"  Storage directory : {DATA_DIR}")
    print(f"  Public base URL   : {PUBLIC_URL}")
    httpd = ThreadingHTTPServer(("0.0.0.0", PORT), ClipHandler)
    httpd.serve_forever()
