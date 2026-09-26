#!/usr/bin/env python3
"""
Smart Greenhouse Training Program — photo-based disease signal.

Runs every growth photo that hasn't been analyzed yet through a pretrained
plant-disease classifier and records what it sees (a label like
"Tomato___Early_blight" or "Strawberry___healthy", plus a confidence score).
train_pooled_model.py folds a recent, confident, non-healthy diagnosis into
a kit's health score as one more rule-based flag, same as it already does
for VPD or soil moisture being out of range.

This is a separate, optional script from train_pooled_model.py because it
needs heavier dependencies (a real deep-learning model), and since photos
are capped at one per kit per day (see dashboard/index.html and
insert_photo in sql/setup.sql), there's no need to run it in real time —
once every hour or two, or once a day, is plenty. Good fits: a Pi cron job,
or run it by hand alongside train_pooled_model.py.

Model: MobileNetV2 fine-tuned on the PlantVillage dataset — 38 classes
across 14 crops, including strawberry, tomato and pepper (this program's
crops): https://huggingface.co/Daksh159/plant-disease-mobilenetv2

Run a smoke test against one uploaded photo and check the printed
label/confidence before relying on this during actual training.

SETUP (once)
------------
1. pip install torch torchvision pillow requests psycopg2-binary
2. Download these two files from the model page above into swarm_model/model/:
     - mobilenetv2_plant.pth   (the trained weights)
     - class_names.json        (maps the model's 38 output indices to labels
                                 like "Tomato___Early_blight" — the dashboard
                                 reads that name straight out of the label
                                 string, so make sure it lists all 38 names
                                 in the model's output order)

RUN
---
    export SUPABASE_URL="https://YOUR-PROJECT-REF.supabase.co"
    export SUPABASE_DB_URL="postgresql://postgres:[password]@[host]:5432/postgres"
    python3 analyze_photos.py

Safe to re-run constantly — it only ever processes photos it hasn't seen
before (photo_analysis.photo_id is the primary key, checked before fetching
the image), so nothing gets analyzed twice.
"""

import io
import json
import os
import sys
from pathlib import Path

import psycopg2
import psycopg2.extras
import requests
from PIL import Image

MODEL_DIR = Path(__file__).parent / "model"
WEIGHTS_PATH = MODEL_DIR / "mobilenetv2_plant.pth"
CLASS_NAMES_PATH = MODEL_DIR / "class_names.json"
MODEL_NAME = "mobilenetv2-plantvillage-38"


def get_connection():
    db_url = os.environ.get("SUPABASE_DB_URL")
    if not db_url:
        sys.exit("SUPABASE_DB_URL is not set (same connection string used by train_pooled_model.py).")
    return psycopg2.connect(db_url)


def load_model_and_classes():
    # Imported lazily so this module can still be loaded/inspected without
    # torch installed (e.g. to test the DB plumbing below on its own).
    import torch
    from torchvision import models

    if not WEIGHTS_PATH.exists() or not CLASS_NAMES_PATH.exists():
        sys.exit(
            f"Missing model files. Download mobilenetv2_plant.pth and class_names.json\n"
            f"from https://huggingface.co/Daksh159/plant-disease-mobilenetv2 into {MODEL_DIR}/\n"
            f"before running this script."
        )
    class_names = json.loads(CLASS_NAMES_PATH.read_text())

    model = models.mobilenet_v2(weights=None)
    # This checkpoint's classifier head is Sequential(ReLU, Linear) rather
    # than a bare Linear — state_dict keys are "classifier.1.1.*". The ReLU
    # adds no weights and is a no-op here (the feature extractor already
    # ends in ReLU6), but the Sequential wrapper is needed to match the
    # checkpoint's shape.
    model.classifier[1] = torch.nn.Sequential(
        torch.nn.ReLU(inplace=True),
        torch.nn.Linear(model.last_channel, len(class_names)),
    )
    state = torch.load(WEIGHTS_PATH, map_location="cpu")
    model.load_state_dict(state)
    model.eval()
    return model, class_names


def classify_image(model, class_names, image_bytes):
    import torch
    from torchvision import transforms

    preprocess = transforms.Compose([
        transforms.Resize((224, 224)),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
    ])
    img = Image.open(io.BytesIO(image_bytes)).convert("RGB")
    x = preprocess(img).unsqueeze(0)
    with torch.no_grad():
        logits = model(x)
        probs = torch.nn.functional.softmax(logits, dim=1)[0]
    idx = int(torch.argmax(probs))
    label = class_names[idx]
    confidence = float(probs[idx])
    is_healthy = "healthy" in label.lower()
    return label, is_healthy, confidence


def fetch_unanalyzed_photos(conn):
    with conn.cursor(cursor_factory=psycopg2.extras.RealDictCursor) as cur:
        cur.execute("""
            select p.id, p.kit_id, p.storage_path
            from photos p
            left join photo_analysis a on a.photo_id = p.id
            where a.photo_id is null
            order by p.uploaded_at
        """)
        return cur.fetchall()


def public_photo_url(supabase_url, bucket, storage_path):
    return f"{supabase_url.rstrip('/')}/storage/v1/object/public/{bucket}/{storage_path}"


def upsert_analysis(conn, photo_id, kit_id, label, is_healthy, confidence, model_name):
    with conn.cursor() as cur:
        cur.execute(
            """
            insert into photo_analysis (photo_id, kit_id, label, is_healthy, confidence, model_name)
            values (%s, %s, %s, %s, %s, %s)
            on conflict (photo_id) do update set
                label = excluded.label, is_healthy = excluded.is_healthy,
                confidence = excluded.confidence, model_name = excluded.model_name,
                analyzed_at = now()
            """,
            (photo_id, kit_id, label, is_healthy, confidence, model_name),
        )
    conn.commit()


def main():
    supabase_url = os.environ.get("SUPABASE_URL")
    bucket = os.environ.get("PHOTOS_BUCKET", "growth-photos")
    if not supabase_url:
        sys.exit("SUPABASE_URL is not set (e.g. https://xxxx.supabase.co — the same value in dashboard/index.html's CONFIG).")

    conn = get_connection()
    try:
        photos = fetch_unanalyzed_photos(conn)
        if not photos:
            print("No new photos to analyze.")
            return

        model, class_names = load_model_and_classes()
        analyzed, failed = 0, 0
        for p in photos:
            url = public_photo_url(supabase_url, bucket, p["storage_path"])
            try:
                resp = requests.get(url, timeout=15)
                resp.raise_for_status()
                label, is_healthy, confidence = classify_image(model, class_names, resp.content)
            except Exception as e:
                print(f"  {p['kit_id']} photo {p['id']}: FAILED to analyze ({e})")
                failed += 1
                continue
            upsert_analysis(conn, p["id"], p["kit_id"], label, is_healthy, confidence, MODEL_NAME)
            print(f"  {p['kit_id']} photo {p['id']}: {label} (confidence {confidence:.2f})")
            analyzed += 1
        print(f"Analyzed {analyzed} photo(s), {failed} failed, out of {len(photos)} new.")
    finally:
        conn.close()


if __name__ == "__main__":
    main()
