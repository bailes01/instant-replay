# InstantReplay Share Server

Lightweight, zero-dependency clip hosting server with per-user galleries (`/u/<username>`), automatic game/application title + formatted timestamp display (no raw filenames shown), HTTP Range video scrubbing, and Discord inline video embeds.

## Storage Location
By default, uploaded clips and metadata are stored in `./data/` (or whatever path you set in `DATA_DIR` inside `server/.env`).
- Set `MAX_STORAGE_GB` (default `50`) to automatically prune the oldest clips if storage ever exceeds your cap.

## Routes
- `POST /api/upload` — Headers: `Authorization: Bearer <token>`, `X-Username: <user>`, `X-App-Name: <game/app>`
- `GET /v/<id>` — Clip player page (shows **Game/Application Name**, **Formatted Date & Time**, and **`@username` gallery badge**, with Discord OpenGraph video tags)
- `GET /u/<username>` — Per-user clip gallery grid with hover video previews
- `GET /f/<id>.mp4` — Raw MP4 stream with HTTP 206 Range support

## Running Locally
```powershell
python .\server\app.py
```

## Secure Self-Hosting on Your Domain (Zero Open Router Ports)
You can use any domain registrar (Namecheap, Squarespace/Google, Porkbun, GoDaddy, etc.) with Cloudflare's **$0 Free plan + Cloudflare Tunnel** so you never open a port on your router or expose your home IP:

1. **Add your domain to Cloudflare (Free)**:
   - Sign in at `https://dash.cloudflare.com` → **Add a domain** → select the **Free ($0)** plan.
   - Copy the 2 Cloudflare nameservers into your domain registrar's DNS settings.
2. **Create a Tunnel**:
   - In the Cloudflare dashboard, open **Zero Trust** → **Networks** → **Tunnels** → **Create a tunnel** → choose **Cloudflared**.
   - Copy the tunnel token (`eyJhIjoi...`).
   - Add a **Public Hostname** route:
     - Subdomain: `clips` (e.g. `clips.yourdomain.com`)
     - Service: `http://clip-server:8080` (or `http://localhost:8080` if running outside Docker)
3. **Start with Docker Compose**:
   Create a `.env` file inside `server/`:
   ```env
   PUBLIC_URL=https://clips.yourdomain.com
   UPLOAD_TOKEN=your-long-random-secret-token
   CLOUDFLARE_TUNNEL_TOKEN=eyJhIjoi...
   ```
   Then run:
   ```bash
   cd server
   docker compose --profile tunnel up -d
   ```
4. **Update `config.ini` in InstantReplay**:
   ```ini
   [upload]
   username = player1
   auto_upload = true
   upload_url = https://clips.yourdomain.com/api/upload
   upload_token = your-long-random-secret-token
   copy_link_to_clipboard = true
   ```
