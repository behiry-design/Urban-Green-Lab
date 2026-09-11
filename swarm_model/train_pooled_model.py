#!/usr/bin/env python3
"""
Smart Greenhouse Training Program — pooled "swarm" health model.

WHAT THIS IS
------------
Every kit reports temperature, humidity and soil moisture to the shared
platform. On its own, one kit's data can't tell you whether a reading is
normal or worrying — there's no baseline to compare against. Pool the
readings from every team's kit together, though, and a shared model can
learn what "normal" looks like across the whole class and flag any kit
that's drifting away from it. That's the "swarm" part: every kit that comes
online makes the model that watches ALL of them a little better.

This script:
  1. Pulls recent sensor readings for every kit from the shared database.
  2. Builds a feature vector per kit (recent averages, trend, and the
     agronomic VPD stress signal used elsewhere in this project).
  3. Fits an IsolationForest on the POOLED features across all kits, so a
     kit is judged against the rest of the class, not a fixed threshold.
  4. Combines that anomaly score with explainable rule-based flags (VPD out
     of band, soil very dry/wet, temperature extreme) into a 0-100 health
     score + status per kit.
  5. Writes the results back so the dashboard can show them.

HOW TO RUN IT
-------------
Run it by hand any time during the training ("did that soil-drying-out demo
change anyone's score?"), or put it on a schedule (cron, a GitHub Action, a
Supabase Edge Function on a timer — the setup guide covers the simplest
option). It's safe to re-run constantly: each run recomputes everything
from scratch and overwrites the previous scores.

    export SUPABASE_DB_URL="postgresql://postgres:[password]@[host]:5432/postgres"
    python3 train_pooled_model.py

Get SUPABASE_DB_URL from Supabase: Project Settings > Database > Connection
string (URI). This script needs the DATABASE connection (not the anon/REST
API) because it needs to write health_scores for every kit at once, which
bypasses the per-kit device_token checks by design — treat this connection
string like a password.

Dependencies: pandas, numpy, scikit-learn, psycopg2-binary
    pip install pandas numpy scikit-learn psycopg2-binary
"""

import os
import sys
import json
from datetime import datetime, timedelta, timezone

import numpy as np
import pandas as pd
import psycopg2
import psycopg2.extras
from sklearn.ensemble import IsolationForest

# ============================================================================
# Tunable settings
# ============================================================================

# Only use readings from the last LOOKBACK_HOURS to compute a kit's current
# status — a kit's health today shouldn't be judged on data from three days
# ago. 24h is a reasonable default for a 3-day training; widen it if kits
# report infrequently.
LOOKBACK_HOURS = 24

# A kit needs at least this many readings in the lookback window before the
# model will score it at all — otherwise a single noisy reading could look
# like a huge outlier.
MIN_READINGS_FOR_SCORING = 3

# Vapor Pressure Deficit target band (kPa) for vegetative-stage growth. This
# is the same agronomic signal used in the project's own greenhouse pilot
# analysis. It's a reasonable general default across common greenhouse fruit
# crops (tomato, pepper, strawberry, cucumber) — narrow it per fruit_type in
# FRUIT_VPD_BANDS below if a team's crop needs a tighter/looser range.
DEFAULT_VPD_BAND = (0.8, 1.2)
FRUIT_VPD_BANDS = {
    # "strawberry": (0.6, 1.0),
    # "cucumber":   (0.7, 1.1),
}

# Soil moisture percent bounds outside of which we raise a rule-based flag,
# regardless of what the anomaly model thinks.
SOIL_TOO_DRY_PCT = 20.0
SOIL_TOO_WET_PCT = 85.0

# If swarm_model/analyze_photos.py has been run (it's optional — see that
# file), a kit's most recent photo diagnosis within this window is folded in
# as one more rule-based flag, same spirit as the VPD/soil rules above. If
# analyze_photos.py has never been run, photo_analysis is just empty and this
# is a complete no-op — nothing here requires it.
PHOTO_SIGNAL_LOOKBACK_HOURS = 72
PHOTO_SIGNAL_CONFIDENCE_THRESHOLD = 0.6
PHOTO_SIGNAL_PENALTY = 20.0

MODEL_VERSION = 1

# ============================================================================
# Data access
# ============================================================================

def get_connection():
    db_url = os.environ.get("SUPABASE_DB_URL")
    if not db_url:
        sys.exit(
            "SUPABASE_DB_URL is not set. Export the Supabase database "
            "connection string first (Project Settings > Database > "
            "Connection string)."
        )
    return psycopg2.connect(db_url)


def fetch_recent_readings(conn, lookback_hours):
    since = datetime.now(timezone.utc) - timedelta(hours=lookback_hours)
    query = """
        select r.kit_id, r.recorded_at, r.temp_c, r.humidity_pct,
               r.soil_moisture_pct, k.fruit_type
        from readings r
        join kits k on k.id = r.kit_id
        where r.recorded_at >= %s
        order by r.kit_id, r.recorded_at
    """
    # Fetched via a plain cursor (rather than pandas.read_sql) so this script
    # only needs psycopg2 — no SQLAlchemy dependency required.
    with conn.cursor() as cur:
        cur.execute(query, (since,))
        cols = [c.name for c in cur.description]
        rows = cur.fetchall()
    df = pd.DataFrame(rows, columns=cols)
    if not df.empty:
        df["recorded_at"] = pd.to_datetime(df["recorded_at"], utc=True)
    return df


def fetch_all_kit_ids(conn):
    with conn.cursor() as cur:
        cur.execute("select id from kits order by id")
        return [row[0] for row in cur.fetchall()]


# ============================================================================
# Feature engineering
# ============================================================================

def compute_vpd_kpa(temp_c, rh_pct):
    """Vapor Pressure Deficit in kPa from air temp (C) and relative humidity (%)."""
    svp = 0.6108 * np.exp((17.27 * temp_c) / (temp_c + 237.3))
    return svp * (1 - rh_pct / 100.0)


def vpd_band_for(fruit_type):
    return FRUIT_VPD_BANDS.get((fruit_type or "").strip().lower(), DEFAULT_VPD_BAND)


def build_features(readings_df):
    """One row per kit: recent averages + trend + VPD-out-of-band fraction."""
    rows = []
    for kit_id, g in readings_df.groupby("kit_id"):
        g = g.sort_values("recorded_at")
        n = len(g)
        fruit_type = g["fruit_type"].iloc[0]
        vpd = compute_vpd_kpa(g["temp_c"], g["humidity_pct"])
        lo, hi = vpd_band_for(fruit_type)
        vpd_out_of_band_frac = float(((vpd < lo) | (vpd > hi)).mean())

        # Soil moisture trend: simple slope over the window (% per hour).
        # Positive = getting wetter, negative = drying out.
        hours = (g["recorded_at"] - g["recorded_at"].iloc[0]).dt.total_seconds() / 3600.0
        if n >= 2 and hours.iloc[-1] > 0:
            soil_slope = float(np.polyfit(hours, g["soil_moisture_pct"], 1)[0])
        else:
            soil_slope = 0.0

        rows.append({
            "kit_id": kit_id,
            "fruit_type": fruit_type,
            "n_readings": n,
            "mean_temp_c": float(g["temp_c"].mean()),
            "mean_humidity_pct": float(g["humidity_pct"].mean()),
            "mean_soil_pct": float(g["soil_moisture_pct"].mean()),
            "mean_vpd_kpa": float(vpd.mean()),
            "vpd_out_of_band_frac": vpd_out_of_band_frac,
            "soil_slope_pct_per_hr": soil_slope,
        })
    return pd.DataFrame(rows)


# ============================================================================
# Pooled anomaly model + explainable rule flags -> health score
# ============================================================================

def score_kits(features_df):
    scorable = features_df[features_df["n_readings"] >= MIN_READINGS_FOR_SCORING].copy()
    results = []

    if len(scorable) >= 2:
        # These are the columns the pooled model actually looks at. Pooling
        # across every kit is what makes this "swarm" rather than per-kit
        # thresholds: with only one kit there's no meaningful "normal" to
        # compare against.
        model_cols = ["mean_temp_c", "mean_humidity_pct", "mean_soil_pct",
                      "mean_vpd_kpa", "soil_slope_pct_per_hr"]
        X = scorable[model_cols].values
        n_estimators = 200
        contamination = "auto"
        clf = IsolationForest(
            n_estimators=n_estimators,
            contamination=contamination,
            random_state=42,
        )
        clf.fit(X)
        # decision_function: higher = more "normal" for this pool, lower/negative
        # = more anomalous. Rescale to 0-100 across the current pool.
        raw = clf.decision_function(X)
        lo, hi = raw.min(), raw.max()
        if hi > lo:
            anomaly_health = 100.0 * (raw - lo) / (hi - lo)
        else:
            anomaly_health = np.full_like(raw, 100.0)  # everyone identical -> no outliers
        scorable["anomaly_health"] = anomaly_health
    else:
        # Not enough kits reporting yet for the pooled model to mean anything.
        scorable["anomaly_health"] = 100.0

    for _, row in scorable.iterrows():
        factors = {}
        rule_penalty = 0.0

        if row["vpd_out_of_band_frac"] > 0.5:
            factors["vpd_out_of_band"] = True
            rule_penalty += 25.0
        if row["mean_soil_pct"] < SOIL_TOO_DRY_PCT:
            factors["soil_dry"] = True
            rule_penalty += 25.0
        if row["mean_soil_pct"] > SOIL_TOO_WET_PCT:
            factors["soil_waterlogged"] = True
            rule_penalty += 20.0
        if row["soil_slope_pct_per_hr"] < -3.0:
            factors["soil_drying_fast"] = True
            rule_penalty += 10.0

        score = max(0.0, min(100.0, row["anomaly_health"] - rule_penalty))

        if score >= 70 and not factors:
            status = "healthy"
        elif score >= 40:
            status = "watch"
        else:
            status = "stressed"

        results.append({
            "kit_id": row["kit_id"],
            "score": round(float(score), 1),
            "status": status,
            "contributing_factors": json.dumps(factors),
        })

    scored_ids = {r["kit_id"] for r in results}
    for _, row in features_df[~features_df["kit_id"].isin(scored_ids)].iterrows():
        results.append({
            "kit_id": row["kit_id"],
            "score": None,
            "status": "insufficient_data",
            "contributing_factors": json.dumps({"n_readings": int(row["n_readings"])}),
        })

    return pd.DataFrame(results)


# ============================================================================
# Optional: fold in the photo-based disease signal (see analyze_photos.py)
# ============================================================================

def fetch_recent_disease_flags(conn, lookback_hours):
    """Most recent photo_analysis row per kit within the lookback window.
    Returns {} if analyze_photos.py has never been run — the table just
    doesn't exist yet or is empty, and the caller treats that as no signal."""
    since = datetime.now(timezone.utc) - timedelta(hours=lookback_hours)
    with conn.cursor() as cur:
        try:
            cur.execute(
                """
                select distinct on (kit_id) kit_id, label, confidence, is_healthy
                from photo_analysis
                where analyzed_at >= %s
                order by kit_id, analyzed_at desc
                """,
                (since,),
            )
        except psycopg2.errors.UndefinedTable:
            conn.rollback()
            return {}
        cols = [c.name for c in cur.description]
        return {row[0]: dict(zip(cols, row)) for row in cur.fetchall()}


def apply_photo_signal(scores_df, disease_flags):
    """Nudges a kit's score/status down if its latest photo looked diseased
    with reasonable confidence. Never touches insufficient_data kits (no
    sensor basis to combine it with) and is a no-op for any kit with no
    recent photo diagnosis, a healthy one, or a low-confidence one."""
    for i, row in scores_df.iterrows():
        flag = disease_flags.get(row["kit_id"])
        if not flag or flag["is_healthy"] or flag["confidence"] < PHOTO_SIGNAL_CONFIDENCE_THRESHOLD:
            continue
        if row["status"] == "insufficient_data":
            continue

        factors = json.loads(row["contributing_factors"])
        factors["disease_suspected"] = True
        scores_df.at[i, "contributing_factors"] = json.dumps(factors)

        if not pd.isna(row["score"]):
            new_score = max(0.0, float(row["score"]) - PHOTO_SIGNAL_PENALTY)
            scores_df.at[i, "score"] = round(new_score, 1)
            # A disease flag can never leave a kit "healthy" — at best "watch".
            if new_score >= 40:
                scores_df.at[i, "status"] = "watch"
            else:
                scores_df.at[i, "status"] = "stressed"
    return scores_df


# ============================================================================
# Write-back
# ============================================================================

def upsert_health_scores(conn, scores_df, model_version):
    with conn.cursor() as cur:
        for _, row in scores_df.iterrows():
            # pandas mixes None into a float64 column as NaN. psycopg2 sends
            # NaN through as the literal float value 'NaN', not SQL NULL —
            # and Postgres's real/float types happily store that. But JSON
            # has no NaN, so PostgREST later fails to serialize the ENTIRE
            # health_scores response for every kit, not just this one. Coerce
            # back to a real Python None so it lands as SQL NULL instead.
            score = None if pd.isna(row["score"]) else float(row["score"])
            cur.execute(
                """
                insert into health_scores (kit_id, updated_at, score, status,
                                            contributing_factors, model_version)
                values (%s, now(), %s, %s, %s::jsonb, %s)
                on conflict (kit_id) do update set
                    updated_at = excluded.updated_at,
                    score = excluded.score,
                    status = excluded.status,
                    contributing_factors = excluded.contributing_factors,
                    model_version = excluded.model_version
                """,
                (row["kit_id"], score, row["status"],
                 row["contributing_factors"], model_version),
            )
    conn.commit()


def update_model_state(conn, n_kits_included, n_readings_included, model_version):
    with conn.cursor() as cur:
        cur.execute(
            """
            update swarm_model_state
            set trained_at = now(),
                n_kits_included = %s,
                n_readings_included = %s,
                model_version = %s,
                notes = %s
            where id = 1
            """,
            (n_kits_included, n_readings_included, model_version,
             f"Pooled IsolationForest + VPD/soil rule flags, lookback={LOOKBACK_HOURS}h"),
        )
    conn.commit()


# ============================================================================
# Main
# ============================================================================

def run_once(conn=None):
    """Does one full scoring pass and returns a small JSON-able summary dict.
    Split out from main() so something other than the command line can
    trigger it directly — e.g. service/app.py, behind the dashboard's
    "Run swarm model now" button — without shelling out to a subprocess.
    Pass an existing connection to reuse it (e.g. a long-lived service);
    otherwise one is opened and closed here, same as running this as a
    script."""
    owns_conn = conn is None
    if owns_conn:
        conn = get_connection()
    try:
        readings_df = fetch_recent_readings(conn, LOOKBACK_HOURS)
        all_kit_ids = fetch_all_kit_ids(conn)

        if readings_df.empty:
            # Still mark every kit as insufficient_data so the dashboard has
            # a row to show instead of nothing at all.
            placeholder = pd.DataFrame([
                {"kit_id": kid, "score": None, "status": "insufficient_data",
                 "contributing_factors": json.dumps({"n_readings": 0})}
                for kid in all_kit_ids
            ])
            upsert_health_scores(conn, placeholder, MODEL_VERSION)
            update_model_state(conn, 0, 0, MODEL_VERSION)
            return {
                "n_kits_total": len(all_kit_ids), "n_kits_scored": 0,
                "n_readings": 0, "statuses": {}, "message": f"No readings in the last {LOOKBACK_HOURS}h yet.",
            }

        features_df = build_features(readings_df)
        # Include kits with zero readings too, so they show "insufficient_data"
        # rather than being silently absent from the dashboard.
        missing = set(all_kit_ids) - set(features_df["kit_id"])
        if missing:
            features_df = pd.concat([
                features_df,
                pd.DataFrame([{"kit_id": kid, "fruit_type": None, "n_readings": 0,
                                "mean_temp_c": np.nan, "mean_humidity_pct": np.nan,
                                "mean_soil_pct": np.nan, "mean_vpd_kpa": np.nan,
                                "vpd_out_of_band_frac": 0.0, "soil_slope_pct_per_hr": 0.0}
                               for kid in missing]),
            ], ignore_index=True)

        scores_df = score_kits(features_df)

        disease_flags = fetch_recent_disease_flags(conn, PHOTO_SIGNAL_LOOKBACK_HOURS)
        if disease_flags:
            scores_df = apply_photo_signal(scores_df, disease_flags)

        upsert_health_scores(conn, scores_df, MODEL_VERSION)

        n_kits_scored = int((scores_df["status"] != "insufficient_data").sum())
        update_model_state(conn, n_kits_scored, len(readings_df), MODEL_VERSION)

        status_counts = scores_df["status"].value_counts().to_dict()
        return {
            "n_kits_total": len(all_kit_ids), "n_kits_scored": n_kits_scored,
            "n_readings": len(readings_df), "statuses": status_counts,
            "message": f"Scored {n_kits_scored}/{len(all_kit_ids)} kits from {len(readings_df)} readings (last {LOOKBACK_HOURS}h).",
            "scores_table": scores_df.sort_values("kit_id").to_string(index=False),
        }
    finally:
        if owns_conn:
            conn.close()


def main():
    result = run_once()
    print(result["message"])
    if "scores_table" in result:
        print(result["scores_table"])


if __name__ == "__main__":
    main()
