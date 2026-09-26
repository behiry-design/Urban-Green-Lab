-- Fix for: "Couldn't delete this photo: Direct deletion from storage tables
-- is not allowed. Use the Storage API instead."
--
-- Cause: delete_photo() was trying to DELETE FROM storage.objects directly
-- in SQL. Supabase blocks that on purpose (it would leave an orphaned file
-- behind), so it has to go through the Storage API instead. This snippet:
--   1. Replaces delete_photo() so it only removes the "photos" metadata row.
--   2. Adds a storage policy that lets the dashboard's own follow-up
--      Storage API call (sb.storage.from(...).remove([path])) actually
--      delete the file — already fixed in the index.html you're redeploying.
--
-- Safe to run as-is on your existing project: replace + add, nothing to
-- drop first.

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

grant execute on function delete_photo(text, text, bigint) to anon, authenticated;

create policy "anyone can delete growth photos"
  on storage.objects for delete
  using (bucket_id = 'growth-photos');
