# Smart Greenhouse Training Platform

A small, self-hosted platform built for a 3-day hands-on greenhouse-monitoring
training: each team wires up an ESP32 kit (temperature, humidity, soil
moisture, optional growth photos), the kit reports straight to a shared
Supabase project, and a live dashboard shows every team's kit side by side —
including a "swarm" model that learns what normal readings look like across
the whole class and flags any kit that's drifting out of range.

**Live dashboard:** https://behiry-design.github.io/greenhouse-training/

Built for a training run with Urban GreenLab in Cairo, Egypt, but everything
here is generic — swap in your own Supabase project and crop list and it
works for any group running the same kind of kit.

## How it fits together

```
ESP32 kit (firmware/)  --HTTPS-->  Supabase (sql/setup.sql)  <--reads/writes--  dashboard/index.html
                                         ^
                                         |  psycopg2 (server-side DB URL, never shipped to a browser)
                                         |
                          swarm_model/train_pooled_model.py   (pooled anomaly-detection model)
                          swarm_model/analyze_photos.py        (optional plant-disease check on growth photos)
                                         ^
                                         |  can be triggered on demand instead of only by cron
                          service/app.py  (tiny FastAPI wrapper, deployed on Render)
```

- **`firmware/`** — Arduino sketches for the ESP32 kits. `kit_firmware.ino` is
  one kit per board; `kit_firmware_2kits.ino` drives two independent sensor
  stations off a single board (used when boards were limited). Each sketch
  reads its sensors on a timer and POSTs a reading straight to Supabase's
  `insert_reading` RPC over HTTPS — no server of your own required.
- **`sql/setup.sql`** — run once in the Supabase SQL editor. Creates the
  tables, the `insert_reading` / `insert_photo` RPCs that check each kit's
  `device_token` before writing anything, and the views the dashboard reads
  from. Row-Level Security blocks any other write path.
- **`dashboard/index.html`** — a single static HTML file (Supabase JS client
  loaded from a CDN, no build step) with two views: an instructor view
  showing every kit at once, and a per-team view a team logs into with just
  their kit ID + device token to watch their own readings, upload a daily
  growth photo, and see their current health status.
- **`swarm_model/train_pooled_model.py`** — pools recent readings across
  every kit, fits an IsolationForest, and writes a health score + status
  (`healthy` / `watch` / `stressed` / `insufficient_data`) back to Supabase.
  Safe to re-run constantly — it recomputes from scratch each time. Meant to
  run on a schedule (cron, GitHub Action, Supabase Edge Function).
- **`swarm_model/analyze_photos.py`** — optional. Runs a MobileNetV2 plant-
  disease classifier over any newly uploaded growth photos and folds a
  confident, non-healthy diagnosis into the health score above.
- **`service/app.py`** — a minimal FastAPI service (deployed on Render) that
  exposes `train_pooled_model.run_once()` as a `/run` endpoint behind a
  shared-secret header, so the dashboard's instructor view can trigger a
  model run on demand instead of waiting for the next scheduled one. Purely
  a convenience — everything else works without it.

## Running your own training with this

1. **Create a Supabase project**, open its SQL editor, and run all of
   `sql/setup.sql` once. Read the comments at the top of that file first —
   it explains the kit_id/device_token model and why writes and reads are
   split the way they are.
2. **Fill in `dashboard/index.html`'s `CONFIG` block** (near the top of the
   `<script>` tag) with your project's URL and anon key, and set
   `PROGRAM_CROPS` to whatever your teams are actually growing. Host the
   file anywhere that serves static HTML — the live dashboard above is just
   this file on GitHub Pages.
3. **Give each team a kit_id and a device_token** (a random secret string
   per kit — anything unguessable works) and insert a row per kit into the
   `kits` table. Keep the full list of tokens in a local file such as
   `kit_roster.csv` and **never commit it** — anyone holding a token can
   write fake readings or delete that team's photos. `.gitignore` already
   excludes the common names for this file.
4. **Flash the firmware** on each kit: open `firmware/kit_firmware.ino` (or
   `kit_firmware_2kits.ino` for a board driving two stations), fill in the
   `CONFIG` block at the top with your WiFi, your Supabase URL/anon key, and
   that kit's own kit_id + device_token, then upload from the Arduino IDE.
5. **Run the swarm model** — either by hand (`python
   swarm_model/train_pooled_model.py`, with `SUPABASE_DB_URL` set to your
   project's connection string) or on a schedule. For on-demand runs from
   the dashboard, deploy `service/app.py` (e.g. to Render — see
   `service/requirements.txt`), set `SUPABASE_DB_URL` and a `RUN_SECRET` on
   that deployment, and point `CONFIG.RUN_MODEL_URL` in the dashboard at it.
6. **(Optional) plant-disease detection** — `swarm_model/analyze_photos.py`
   downloads its model weights on first run (see that file's own docstring
   for the exact source) and needs a working `SUPABASE_DB_URL` the same way
   the pooled model does.

The repo also includes `Training_Platform_Setup_Guide.docx`, a longer
step-by-step walkthrough of the same setup written for instructors running
the training live.

## Security notes

- `SUPABASE_ANON_KEY` in the dashboard is Supabase's public anon key — safe
  to ship in static, client-side code. It's deliberately weak on its own:
  every write still has to pass the per-kit `device_token` check inside the
  SQL functions in `sql/setup.sql`.
- `SUPABASE_DB_URL` / `RUN_SECRET` (used by `swarm_model/` and
  `service/app.py`) are real secrets — they bypass the per-kit token checks
  by design, since the model needs to read and score every kit at once. Set
  them as environment variables; never hardcode them into a file that gets
  committed.
- A team's `device_token` is effectively that kit's password. Share it only
  with that team, and keep any roster file listing all of them out of
  version control.

## Credits

Implemented by Nature Eye Labs, funded by Green Grants, for the Urban
GreenLab training program in Cairo, Egypt.
