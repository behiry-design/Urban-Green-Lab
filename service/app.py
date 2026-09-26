"""
Small hosted endpoint that lets the dashboard's instructor view trigger the
swarm health model on demand ("Run swarm model now"), without ever putting
the database's admin connection string in the browser — that string can read
AND write every table in the project, so it must never live in client-side
JavaScript (anyone viewing page source would get it).

This service holds that connection string as a server-side secret instead,
and only runs the model when the caller supplies a separate, much lower-
stakes shared secret (RUN_SECRET) — worst case if that one leaks, someone can
re-run the model early; they can't read or change anything.

ENVIRONMENT VARIABLES (set these on whatever host runs this, never in code)
----------------------------------------------------------------------------
SUPABASE_DB_URL  - same Postgres connection string train_pooled_model.py
                   uses (Supabase: Project Settings > Database > Connection
                   string (URI))
RUN_SECRET       - a password you make up yourself; only the dashboard,
                   via CONFIG.RUN_MODEL_SECRET, needs to know it

DEPLOY (Render.com free tier is a reasonable, no-cost option)
----------------------------------------------------------------------------
1. Push this project (needs both service/ and swarm_model/, sitting side by
   side) to a GitHub repo.
2. In Render: New -> Web Service -> connect that repo.
     Root Directory:  LEAVE BLANK (repo root) — do NOT set it to "service".
                       Render restricts a service to seeing only files under
                       its Root Directory at build AND run time, and this
                       file needs to reach the sibling swarm_model/ folder,
                       so the service needs the whole repo checked out.
     Build Command:   pip install -r service/requirements.txt
     Start Command:   uvicorn service.app:app --host 0.0.0.0 --port $PORT
3. Add SUPABASE_DB_URL and RUN_SECRET under the service's Environment tab.
4. Render gives you a URL like https://your-service.onrender.com — put that
   in dashboard/index.html's CONFIG.RUN_MODEL_URL, and the SAME RUN_SECRET
   value in CONFIG.RUN_MODEL_SECRET (yes, that means it's visible in the
   dashboard's JS too — that's fine, it can only trigger a re-score, per the
   worst-case note above; it is NOT the database credential).

Render's free web services spin down after 15 minutes with no requests, and
take about a minute to wake back up on the next one — the dashboard's button
already accounts for this with a patient loading state, so a slow first
click after a quiet spell is expected, not a bug. The free tier includes 750
instance-hours a month, far more than an occasional on-demand button needs.
"""
import os
import sys
from pathlib import Path

from fastapi import FastAPI, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "swarm_model"))
from train_pooled_model import run_once  # noqa: E402

RUN_SECRET = os.environ.get("RUN_SECRET")

app = FastAPI()

# Wide open rather than pinned to one dashboard URL — kept simple for a
# short internal training. Tighten allow_origins to your actual dashboard
# URL if you want.
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["POST", "GET"],
    allow_headers=["*"],
)


@app.get("/")
def health():
    return {"ok": True, "service": "greenhouse-swarm-model-runner"}


@app.post("/run")
def run(x_run_secret: str = Header(default=None)):
    if not RUN_SECRET:
        raise HTTPException(500, "Server misconfigured: RUN_SECRET is not set.")
    if x_run_secret != RUN_SECRET:
        raise HTTPException(401, "Invalid or missing run secret.")
    try:
        result = run_once()
    except Exception as e:
        raise HTTPException(500, f"Model run failed: {e}")
    result.pop("scores_table", None)  # full detail is already visible via health_scores in the dashboard
    return result
