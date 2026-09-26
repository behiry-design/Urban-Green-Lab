-- TEST DATA ONLY — generates 24 hourly fake sensor readings (last 24h) for
-- every kit currently in the `kits` table, so you can test the dashboard
-- charts, health scoring, and the "Run swarm model now" button before any
-- real kit has sent real data.
--
-- Safe to run more than once (it just adds more fake rows each time) and
-- safe to run in Supabase's SQL editor directly — this bypasses the
-- device_token check entirely because it inserts directly into the table
-- as the database owner, not through insert_reading(). Real kits in the
-- field must still go through insert_reading() with their real token.
--
-- To remove this fake data later, see the DELETE statement at the bottom
-- (commented out) — run it once you have real readings coming in.

insert into readings (kit_id, recorded_at, temp_c, humidity_pct, soil_moisture_pct, light_lux)
select
  k.id,
  now() - (h || ' hours')::interval,
  -- temp: ~24-29°C, a gentle day/night wave plus small per-kit offset and noise
  26 + 2 * sin(h / 24.0 * 2 * pi()) + (abs(hashtext(k.id)) % 5 - 2) + (random() - 0.5),
  -- humidity: ~45-70%
  58 + 8 * cos(h / 24.0 * 2 * pi()) + (random() - 0.5) * 6,
  -- soil moisture: ~25-55%, drifts over the day (simulating drying out / watering)
  35 + (h % 8) * 2 + (random() - 0.5) * 5,
  -- light: rough day/night cycle, 0 at "night" hours
  greatest(0, 6000 * sin(((h % 24) / 24.0) * pi()))
from kits k
cross join generate_series(0, 23) as h
order by k.id, h desc;

-- To wipe just this fake data later (once real kits are reporting), run:
-- delete from readings where extra is null and recorded_at > now() - interval '2 days';
-- (Safer/more precise: delete by id range if you note the max id before running this script.)
