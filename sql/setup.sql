-- ============================================================================
-- Smart Greenhouse Training Program — Supabase setup script
-- ============================================================================
-- Run this ONCE in your Supabase project's SQL editor (Project → SQL Editor →
-- New query → paste all of this → Run). It creates everything the dashboard,
-- the ESP32 kits, and the pooled "swarm" health model need.
--
-- Design notes (read this before you run it):
--   * There is no per-student login. Each team's kit gets a short kit_id
--     (e.g. "K01") and a device_token (a random secret string). The firmware
--     and the team's browser both hold that pair — it's how we tell teams
--     apart without spending training time teaching OAuth/accounts.
--   * All writes (sensor readings, photo metadata) go through a handful of
--     SQL functions ("RPCs") that check the device_token before writing
--     anything. Direct table inserts are blocked by Row-Level Security (RLS).
--   * All reads (for the dashboard) are open to anyone with the project's
--     anon key — that's normal for a Supabase project and is NOT a secret
--     key. A view (kits_public) hides device_token from anything readable.
-- ============================================================================

-- ---------------------------------------------------------------------------
-- 1. Tables
-- ---------------------------------------------------------------------------

create table if not exists kits (
  id            text primary key,                 -- e.g. 'K01'
  team_name     text not null,
  fruit_type    text,                              -- e.g. 'tomato', 'strawberry'
  device_token  text not null,                     -- shared secret: firmware + team's browser
  created_at    timestamptz not null default now()
);

create table if not exists readings (
  id                bigint generated always as identity primary key,
  kit_id            text not null references kits(id) on delete cascade,
  recorded_at       timestamptz not null default now(),
  temp_c            real,
  humidity_pct      real,
  soil_moisture_pct real,
  light_lux         real,          -- nullable — leave out if a kit has no light sensor
  extra             jsonb          -- room for anything else a team's kit measures
);

create index if not exists readings_kit_time_idx
  on readings (kit_id, recorded_at desc);

create table if not exists photos (
  id            bigint generated always as identity primary key,
  kit_id        text not null references kits(id) on delete cascade,
  uploaded_at   timestamptz not null default now(),
  storage_path  text not null,     -- path inside the 'growth-photos' storage bucket
  note          text
);

create index if not exists photos_kit_time_idx
  on photos (kit_id, uploaded_at desc);

create table if not exists health_scores (
  kit_id                text primary key references kits(id) on delete cascade,
  updated_at            timestamptz not null default now(),
  score                 real,      -- 0-100, higher = healthier
  status                text,      -- 'healthy' | 'watch' | 'stressed' | 'insufficient_data'
  contributing_factors  jsonb,     -- e.g. {"vpd_high": true, "soil_dry": false}
  model_version         int
);

-- Singleton row tracking the latest pooled ("swarm") model training run
create table if not exists swarm_model_state (
  id                  int primary key default 1,
  trained_at          timestamptz,
  n_kits_included     int,
  n_readings_included int,
  model_version       int,
  notes               text,
  constraint swarm_model_state_singleton check (id = 1)
);
insert into swarm_model_state (id) values (1) on conflict (id) do nothing;

-- One row per photo that's been run through the pretrained plant-disease
-- classifier (see swarm_model/analyze_photos.py). Read-only from the
-- dashboard's perspective — only the trusted backend (holding the DB
-- connection, same as health_scores) ever writes here.
create table if not exists photo_analysis (
  photo_id     bigint primary key references photos(id) on delete cascade,
  kit_id       text not null references kits(id) on delete cascade,
  label        text,       -- e.g. 'Tomato___Early_blight', 'Strawberry___healthy'
  is_healthy   boolean,
  confidence   real,       -- 0-1, the model's confidence in `label`
  model_name   text,
  analyzed_at  timestamptz not null default now()
);
create index if not exists photo_analysis_kit_idx on photo_analysis (kit_id, analyzed_at desc);

-- A view for the dashboard: everything about a kit EXCEPT its secret token
create or replace view kits_public as
  select id, team_name, fruit_type, created_at from kits;

-- ---------------------------------------------------------------------------
-- 2. Row-Level Security — lock the tables down, open the views/functions up
-- ---------------------------------------------------------------------------

alter table kits              enable row level security;
alter table readings          enable row level security;
alter table photos            enable row level security;
alter table health_scores     enable row level security;
alter table swarm_model_state enable row level security;
alter table photo_analysis    enable row level security;

-- No direct policies are created on kits/readings/photos for INSERT/UPDATE/
-- DELETE — with RLS enabled and no policy, those are blocked entirely, even
-- for the anon key. All writes must go through the security-definer
-- functions below, which check the device_token themselves.

-- Reads: dashboard needs to read readings, photos, health_scores, and the
-- public kit list. It never reads the kits table directly (that's where
-- device_token lives) — it reads kits_public instead.
create policy "public read readings"      on readings          for select using (true);
create policy "public read photos"        on photos            for select using (true);
create policy "public read health_scores" on health_scores     for select using (true);
create policy "public read swarm_state"   on swarm_model_state for select using (true);
create policy "public read photo_analysis" on photo_analysis   for select using (true);
-- Intentionally NO select policy on kits itself — only kits_public is exposed.
-- Intentionally NO write policy on photo_analysis — only analyze_photos.py,
-- connecting with the DB URL (not the anon key), writes here.
grant select on kits_public to anon, authenticated;
grant select on readings, photos, health_scores, swarm_model_state, photo_analysis to anon, authenticated;

-- ---------------------------------------------------------------------------
-- 3. Token-checked write functions (what the firmware and dashboard call)
-- ---------------------------------------------------------------------------

-- Verifies a (kit_id, device_token) pair. Raises an exception if it doesn't match.
create or replace function assert_kit_token(p_kit_id text, p_token text)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
  if not exists (
    select 1 from kits where id = p_kit_id and device_token = p_token
  ) then
    raise exception 'Invalid kit_id/device_token for kit %', p_kit_id
      using errcode = '28000'; -- invalid_authorization_specification
  end if;
end;
$$;

-- Called by the ESP32 firmware on a timer, e.g. every 5-15 minutes.
create or replace function insert_reading(
  p_kit_id      text,
  p_token       text,
  p_temp_c      real default null,
  p_humidity    real default null,
  p_soil        real default null,
  p_light       real default null,
  p_extra       jsonb default null
)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
  perform assert_kit_token(p_kit_id, p_token);
  insert into readings (kit_id, temp_c, humidity_pct, soil_moisture_pct, light_lux, extra)
  values (p_kit_id, p_temp_c, p_humidity, p_soil, p_light, p_extra);
end;
$$;

-- Called by the dashboard AFTER the browser uploads the photo file itself to
-- the 'growth-photos' storage bucket — this just registers the metadata row
-- that makes the photo show up in the gallery/timeline.
create or replace function insert_photo(
  p_kit_id       text,
  p_token        text,
  p_storage_path text,
  p_note         text default null
)
returns void
language plpgsql
security definer
set search_path = public
as $$
declare
  v_last_uploaded_at timestamptz;
  -- How often a team may add a growth photo. Once a day is enough to show
  -- real change without teams dumping a burst of near-identical shots — this
  -- is the ACTUAL limit; the dashboard's own countdown is just a friendlier
  -- front end for it and can't be relied on alone. Change the interval here
  -- (and the matching CONFIG.PHOTO_MIN_INTERVAL_HOURS in dashboard/index.html,
  -- so the on-screen countdown stays accurate) if daily is too fast or slow
  -- for how quickly your crop visibly changes.
  v_min_interval interval := interval '24 hours';
begin
  perform assert_kit_token(p_kit_id, p_token);

  select max(uploaded_at) into v_last_uploaded_at from photos where kit_id = p_kit_id;
  if v_last_uploaded_at is not null and now() - v_last_uploaded_at < v_min_interval then
    raise exception 'Too soon since the last growth photo for this kit — next one allowed after %',
      to_char(v_last_uploaded_at + v_min_interval, 'YYYY-MM-DD HH24:MI')
      using errcode = 'P0001';
  end if;

  insert into photos (kit_id, storage_path, note)
  values (p_kit_id, p_storage_path, p_note);
end;
$$;

-- Called by the dashboard when a team deletes one of their own photos (e.g.
-- a blurry or off-topic shot). Token-checked like every other write here.
-- This ONLY removes the metadata row — Supabase deliberately blocks direct
-- SQL DELETE on storage.objects ("Direct deletion from storage tables is
-- not allowed. Use the Storage API instead"), because deleting that row
-- without going through its Storage API would leave the actual file behind
-- as an orphan. So the dashboard does the real file removal itself right
-- after this call succeeds, via sb.storage.from(...).remove([path]) — see
-- deletePhoto() in dashboard/index.html. The storage policy below is what
-- lets that client-side call succeed.
create or replace function delete_photo(
  p_kit_id   text,
  p_token    text,
  p_photo_id bigint
)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
  perform assert_kit_token(p_kit_id, p_token);

  delete from photos where id = p_photo_id and kit_id = p_kit_id;
  if not found then
    raise exception 'Photo % not found for kit %', p_photo_id, p_kit_id;
  end if;
end;
$$;

-- Called by the dashboard when a team sets or changes what they're growing.
-- Teams pick their own fruit/plant type when they get their kit — the
-- instructor doesn't need to know it in advance when registering kits.
create or replace function set_kit_fruit_type(
  p_kit_id      text,
  p_token       text,
  p_fruit_type  text
)
returns void
language plpgsql
security definer
set search_path = public
as $$
begin
  perform assert_kit_token(p_kit_id, p_token);
  update kits set fruit_type = p_fruit_type where id = p_kit_id;
end;
$$;

grant execute on function assert_kit_token(text, text) to anon, authenticated;
grant execute on function insert_reading(text, text, real, real, real, real, jsonb) to anon, authenticated;
grant execute on function insert_photo(text, text, text, text) to anon, authenticated;
grant execute on function delete_photo(text, text, bigint) to anon, authenticated;
grant execute on function set_kit_fruit_type(text, text, text) to anon, authenticated;

-- The pooled/"swarm" retraining script (Python, run on a schedule by the
-- instructor — see swarm_model/train_pooled_model.py) writes health_scores
-- and swarm_model_state using the Supabase SERVICE ROLE key, which bypasses
-- RLS entirely — it does not need a token-checked function.

-- ---------------------------------------------------------------------------
-- 4. Storage bucket for growth photos
-- ---------------------------------------------------------------------------

-- file_size_limit is a hard server-side cap (bytes) — belt-and-suspenders
-- alongside the dashboard's own in-browser resize-before-upload step, so a
-- lot of teams' phone photos can't quietly blow through the free-tier
-- storage quota even if something skips the client-side compression.
insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
values ('growth-photos', 'growth-photos', true, 5242880, array['image/jpeg','image/png','image/webp'])
on conflict (id) do nothing;

-- Anyone can upload (kept simple for a short internal training — see the
-- setup guide's "Scope & limitations" note) and anyone can read (so <img>
-- tags on the dashboard load without extra auth headers). Delete is
-- similarly open at the storage layer — the real "only your own kit" gate
-- is delete_photo()'s device_token check just above, which removes the
-- metadata row; a deleted photo already vanishes from the gallery there.
-- This policy just lets the dashboard's follow-up Storage API call actually
-- remove the file, which Supabase requires going through this API for
-- rather than a raw SQL DELETE (see the comment on delete_photo() above).
create policy "anyone can upload growth photos"
  on storage.objects for insert
  with check (bucket_id = 'growth-photos');

create policy "anyone can read growth photos"
  on storage.objects for select
  using (bucket_id = 'growth-photos');

create policy "anyone can delete growth photos"
  on storage.objects for delete
  using (bucket_id = 'growth-photos');

-- ---------------------------------------------------------------------------
-- 5. Register your teams' kits
-- ---------------------------------------------------------------------------
-- Run one insert per team (15-30 rows). Generate a random device_token for
-- each — e.g. in a terminal: python3 -c "import secrets; print(secrets.token_hex(8))"
-- Keep this list of kit_id/device_token pairs private — hand each team ONLY
-- its own pair (printed on a card, or in their firmware config file).
--
-- insert into kits (id, team_name, fruit_type, device_token) values
--   ('K01', 'Team Falcon',  'strawberry', 'REPLACE_WITH_RANDOM_TOKEN_1'),
--   ('K02', 'Team Basil',   'tomato',     'REPLACE_WITH_RANDOM_TOKEN_2');
--   -- ... one row per team, up to K30
