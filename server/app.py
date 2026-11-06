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
import socket
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
    raw_ratings = meta.get("ratings", {})
    ratings: dict[str, float] = {}
    if isinstance(raw_ratings, dict):
        for rk, rv in raw_ratings.items():
            try:
                val = float(rv)
                if 0.0 <= val <= 5.0:
                    ratings[str(rk)[:64]] = round(val, 2)
            except (ValueError, TypeError):
                pass
    rating_count = len(ratings)
    rating_avg = round(sum(ratings.values()) / rating_count, 2) if rating_count > 0 else 0.0
    return {
        "id": clip_id,
        "username": user,
        "app_name": app_name,
        "title": title,
        "public": is_public,
        "ratings": ratings,
        "rating_avg": rating_avg,
        "rating_count": rating_count,
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


SERVER_DIR = Path(__file__).resolve().parent
TEMPLATES_DIR = SERVER_DIR / "templates"
STATIC_DIR = SERVER_DIR / "static"


def render_template(name: str, **kwargs) -> str:
    tpl = (TEMPLATES_DIR / name).read_text("utf-8")
    return tpl.format(**kwargs)


def render_star_pill(c: dict) -> str:
    cnt = c.get("rating_count", 0)
    avg = c.get("rating_avg", 0.0)
    pct = max(0.0, min(100.0, (avg / 5.0) * 100.0))
    txt = f"{avg:.2f}/5 ({cnt})" if cnt > 0 else "No ratings"
    return f"""<div class="rating-pill" title="{txt}">
      <span class="stars-wrap"><span class="stars-bg">&#9733;&#9733;&#9733;&#9733;&#9733;</span><span class="stars-fg" style="width:{pct:.1f}%">&#9733;&#9733;&#9733;&#9733;&#9733;</span></span>
      <span class="rating-txt">{txt}</span>
    </div>"""


def sort_clip_list(lst: list[dict], sort_mode: str) -> list[dict]:
    if sort_mode == "lowest":
        return sorted(
            lst,
            key=lambda x: (
                0 if x.get("rating_count", 0) > 0 else 1,
                x.get("rating_avg", 0.0),
                -x.get("rating_count", 0),
                -x.get("created_at", 0),
            ),
        )
    if sort_mode == "most_rated":
        return sorted(
            lst,
            key=lambda x: (x.get("rating_count", 0), x.get("rating_avg", 0.0), x.get("created_at", 0)),
            reverse=True,
        )
    if sort_mode == "oldest":
        return sorted(lst, key=lambda x: x.get("created_at", 0))
    if sort_mode == "newest":
        return sorted(lst, key=lambda x: x.get("created_at", 0), reverse=True)
    # Default: highest / top rated
    return sorted(
        lst,
        key=lambda x: (
            1 if x.get("rating_count", 0) > 0 else 0,
            x.get("rating_avg", 0.0),
            x.get("rating_count", 0),
            x.get("created_at", 0),
        ),
        reverse=True,
    )


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

    def _get_rater_id(self) -> tuple[str, str | None]:
        raw_cookie = self.headers.get("Cookie", "")
        if raw_cookie:
            c = cookies.SimpleCookie()
            try:
                c.load(raw_cookie)
                # Prefer authenticated user cookie if present
                for k, morsel in c.items():
                    if k.startswith("ir_token_"):
                        u = sanitize_username(k[len("ir_token_"):])
                        if check_user_token_readonly(u, morsel.value):
                            return f"user:{u}", None
                if "ir_rater_id" in c and ID_RE.match(c["ir_rater_id"].value):
                    return f"anon:{c['ir_rater_id'].value}", None
            except Exception:
                pass
        new_id = secrets.token_urlsafe(9)[:12]
        cookie_hdr = f"ir_rater_id={new_id}; Path=/; Max-Age=31536000; HttpOnly; SameSite=Lax"
        return f"anon:{new_id}", cookie_hdr

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

        if path == "/static/styles.css":
            css_file = STATIC_DIR / "styles.css"
            if not css_file.is_file():
                self.send_error(404)
                return
            data = css_file.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/css; charset=utf-8")
            self.send_header("Cache-Control", "public, max-age=60")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self._write_body(data)
            return

        # Root or /gallery -> Public community gallery
        if path in ("/", "/gallery"):
            sort_mode = qs.get("sort", ["rating"])[0].lower()
            clips = load_all_clips(username_filter=None, include_private=False)

            by_player: dict[str, list[dict]] = {}
            for c in clips:
                by_player.setdefault(c["username"], []).append(c)

            def player_stats(pclips: list[dict]) -> tuple[float, int, int]:
                rated = [x for x in pclips if x.get("rating_count", 0) > 0]
                total_votes = sum(x["rating_count"] for x in rated)
                weighted_sum = sum(x["rating_avg"] * x["rating_count"] for x in rated)
                p_avg = round(weighted_sum / total_votes, 2) if total_votes > 0 else 0.0
                newest_ts = max((x.get("created_at", 0) for x in pclips), default=0)
                return p_avg, total_votes, newest_ts

            if sort_mode == "lowest":
                sorted_players = sorted(
                    by_player.items(),
                    key=lambda item: (
                        0 if player_stats(item[1])[1] > 0 else 1,
                        player_stats(item[1])[0],
                        -player_stats(item[1])[2],
                        item[0],
                    ),
                )
            elif sort_mode == "most_rated":
                sorted_players = sorted(
                    by_player.items(),
                    key=lambda item: (player_stats(item[1])[1], player_stats(item[1])[0], player_stats(item[1])[2], item[0]),
                    reverse=True,
                )
            elif sort_mode == "oldest":
                sorted_players = sorted(
                    by_player.items(),
                    key=lambda item: (min((x.get("created_at", 0) for x in item[1]), default=0), item[0]),
                )
            elif sort_mode == "newest":
                sorted_players = sorted(
                    by_player.items(),
                    key=lambda item: (player_stats(item[1])[2], player_stats(item[1])[0], item[0]),
                    reverse=True,
                )
            else:
                sorted_players = sorted(
                    by_player.items(),
                    key=lambda item: (
                        1 if player_stats(item[1])[1] > 0 else 0,
                        player_stats(item[1])[0],
                        player_stats(item[1])[1],
                        player_stats(item[1])[2],
                        item[0],
                    ),
                    reverse=True,
                )

            sections_html = []
            for idx, (u_raw, pclips) in enumerate(sorted_players):
                u = html.escape(u_raw)
                pclips_sorted = sort_clip_list(pclips, sort_mode)
                player_avg, total_votes, _ = player_stats(pclips_sorted)
                if total_votes > 0:
                    player_pct = max(0.0, min(100.0, (player_avg / 5.0) * 100.0))
                    player_rating_badge = f"""<span class="player-rating">
                      <span class="stars-wrap"><span class="stars-bg">&#9733;&#9733;&#9733;&#9733;&#9733;</span><span class="stars-fg" style="width:{player_pct:.1f}%">&#9733;&#9733;&#9733;&#9733;&#9733;</span></span>
                      <strong>{player_avg:.2f}/5</strong> <span class="vote-cnt">({total_votes} rating{"s" if total_votes != 1 else ""})</span>
                    </span>"""
                else:
                    player_rating_badge = '<span class="player-rating unrated">Unrated</span>'

                cards_html = []
                for c in pclips_sorted:
                    cid = html.escape(c["id"])
                    title = html.escape(c["title"])
                    fdate = html.escape(c["formatted_date"])
                    size_mb = c.get("size", 0) / (1024 * 1024)
                    star_pill = render_star_pill(c)
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
                        <div class="bottom-row">
                          <span class="sub">{fdate} &middot; {size_mb:.1f} MB</span>
                          {star_pill}
                        </div>
                      </div>
                    </div>""")

                divider_html = '<hr class="player-divider">' if idx > 0 else ""
                sections_html.append(f"""
                {divider_html}
                <section class="player-section">
                  <div class="player-header">
                    <div class="player-title-grp">
                      <a class="player-name" href="/u/{u}">@{u}</a>
                      {player_rating_badge}
                    </div>
                    <span class="count">{len(pclips_sorted)} clip{"s" if len(pclips_sorted) != 1 else ""}</span>
                  </div>
                  <div class="grid">{''.join(cards_html)}</div>
                </section>""")

            empty_msg = '<p class="empty">No public clips yet. Owners can mark clips as Public from their gallery.</p>' if not sections_html else ""
            page = render_template(
                "gallery.html",
                active_rating="active" if sort_mode not in ("lowest", "most_rated", "newest", "oldest") else "",
                active_lowest="active" if sort_mode == "lowest" else "",
                active_most="active" if sort_mode == "most_rated" else "",
                active_newest="active" if sort_mode == "newest" else "",
                active_oldest="active" if sort_mode == "oldest" else "",
                clip_count_label=f"{len(clips)} public clip{'s' if len(clips) != 1 else ''}",
                empty_msg=empty_msg,
                sections_html="".join(sections_html),
            )
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
            sort_mode = qs.get("sort", ["newest"])[0].lower()
            clips = sort_clip_list(load_all_clips(username_filter=user, include_private=is_owner), sort_mode)
            esc_user = html.escape(user)
            rated_user_clips = [x for x in clips if x.get("rating_count", 0) > 0]
            if rated_user_clips:
                u_votes = sum(x["rating_count"] for x in rated_user_clips)
                u_wsum = sum(x["rating_avg"] * x["rating_count"] for x in rated_user_clips)
                u_avg = u_wsum / u_votes if u_votes > 0 else 0.0
                u_pct = max(0.0, min(100.0, (u_avg / 5.0) * 100.0))
                owner_badge = f"""<span class="player-rating">
                  <span class="stars-wrap"><span class="stars-bg">&#9733;&#9733;&#9733;&#9733;&#9733;</span><span class="stars-fg" style="width:{u_pct:.1f}%">&#9733;&#9733;&#9733;&#9733;&#9733;</span></span>
                  <strong>{u_avg:.2f}/5</strong> <span class="vote-cnt">({u_votes} rating{"s" if u_votes != 1 else ""})</span>
                </span>"""
            else:
                owner_badge = '<span class="player-rating unrated">Unrated</span>'

            cards_html = []
            for c in clips:
                cid = html.escape(c["id"])
                title = html.escape(c["title"])
                fdate = html.escape(c["formatted_date"])
                size_mb = c.get("size", 0) / (1024 * 1024)
                star_pill = render_star_pill(c)
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
                      <div class="card-right-meta">
                        {star_pill}
                        {edit_btn}
                      </div>
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
            page = render_template(
                "user_gallery.html",
                esc_user=esc_user,
                owner_badge=owner_badge,
                u_act_rating="active" if sort_mode == "rating" else "",
                u_act_lowest="active" if sort_mode == "lowest" else "",
                u_act_most="active" if sort_mode == "most_rated" else "",
                u_act_newest="active" if sort_mode not in ("rating", "lowest", "most_rated", "oldest") else "",
                u_act_oldest="active" if sort_mode == "oldest" else "",
                clip_count_label=f"{len(clips)} clip{'s' if len(clips) != 1 else ''}",
                empty_msg=empty_msg,
                cards_html="".join(cards_html),
            )
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
            rater_id, new_rater_cookie = self._get_rater_id()
            my_rating = meta["ratings"].get(rater_id)
            rating_avg = meta["rating_avg"]
            rating_count = meta["rating_count"]
            rating_pct = max(0.0, min(100.0, (rating_avg / 5.0) * 100.0))
            rating_summary_txt = f"{rating_avg:.2f}/5 ({rating_count} rating{'s' if rating_count != 1 else ''})" if rating_count > 0 else "No ratings yet"
            my_rating_txt = f"Your rating: {my_rating:.1f}/5" if my_rating is not None else "Click or drag stars to rate (0.5 steps)"

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
                editor_html = render_template(
                    "editor.html",
                    clip_id=clip_id,
                    esc_title=esc_title,
                    esc_user=esc_user,
                    pub_checked="checked" if is_pub else "",
                    priv_checked="" if is_pub else "checked",
                )

            page = render_template(
                "clip.html",
                og_title=og_title,
                og_desc=og_desc,
                esc_user=esc_user,
                page_url=page_url,
                thumb_url=thumb_url,
                raw_url=raw_url,
                clip_id=clip_id,
                mtime=mtime,
                esc_title=esc_title,
                vis_pill=vis_pill,
                esc_date=esc_date,
                esc_app=esc_app,
                rating_pct=f"{rating_pct:.1f}",
                rating_summary_txt=rating_summary_txt,
                my_rating_txt=my_rating_txt,
                edit_toggle_btn=edit_toggle_btn,
                editor_html=editor_html,
            )
            extra_hdrs = {"Set-Cookie": new_rater_cookie} if new_rater_cookie else None
            self._send_html(200, page, extra_headers=extra_hdrs)
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

        # Submit or update a decimal 0.00 - 5.00 star rating on a clip
        if path.startswith("/api/clips/") and path.endswith("/rate"):
            clip_id = path[len("/api/clips/"):-len("/rate")].strip("/")
            meta = load_clip_meta(clip_id)
            if not meta:
                self._send_json(404, {"error": "clip not found"})
                return
            body = self._read_json_body()
            if not body or "rating" not in body:
                self._send_json(400, {"error": "rating required"})
                return
            try:
                val = round(float(body["rating"]) * 2.0) / 2.0
            except (ValueError, TypeError):
                self._send_json(400, {"error": "invalid rating value"})
                return
            if val < 0.5 or val > 5.0:
                self._send_json(400, {"error": "rating must be between 0.5 and 5.0 in 0.5 steps"})
                return

            rater_id, new_cookie = self._get_rater_id()
            ratings = dict(meta.get("ratings", {}))
            ratings[rater_id] = val
            meta["ratings"] = ratings
            meta["rating_count"] = len(ratings)
            meta["rating_avg"] = round(sum(ratings.values()) / len(ratings), 2)
            save_clip_meta(meta)

            payload = {
                "id": clip_id,
                "my_rating": val,
                "rating_avg": meta["rating_avg"],
                "rating_count": meta["rating_count"],
            }
            resp_bytes = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(resp_bytes)))
            if new_cookie:
                self.send_header("Set-Cookie", new_cookie)
            self.end_headers()
            self._write_body(resp_bytes)
            return

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

        # Authenticate via global UPLOAD_TOKEN and/or per-user token:
        # 1. If a valid global UPLOAD_TOKEN is presented, always allow the upload AND
        #    register/claim `username` with `user_token` if `username` isn't claimed yet.
        # 2. Otherwise, allow if `user_token` matches `username` (or claims an unclaimed username).
        authed = False
        has_valid_global_token = False
        if UPLOAD_TOKEN:
            auth_hdr = self.headers.get("Authorization", "")
            if secrets.compare_digest(auth_hdr, f"Bearer {UPLOAD_TOKEN}"):
                has_valid_global_token = True
                authed = True

        if user_token and verify_or_register_user_token(username, user_token):
            authed = True
        elif not has_valid_global_token:
            authed = False

        if not authed:
            self._send_json(401, {"error": "unauthorized (invalid upload_token or username claimed by another user_token)"})
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


class DualStackThreadingHTTPServer(ThreadingHTTPServer):
    address_family = socket.AF_INET6
    daemon_threads = True

    def server_bind(self):
        try:
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        except Exception:
            pass
        super().server_bind()


if __name__ == "__main__":
    print(f"InstantReplay Share Server v2.0 listening on http://[::]:{PORT} (IPv4 + IPv6)")
    print(f"  Storage directory : {DATA_DIR}")
    print(f"  Public base URL   : {PUBLIC_URL}")
    try:
        httpd = DualStackThreadingHTTPServer(("::", PORT), ClipHandler)
    except Exception:
        httpd = ThreadingHTTPServer(("0.0.0.0", PORT), ClipHandler)
    httpd.serve_forever()
