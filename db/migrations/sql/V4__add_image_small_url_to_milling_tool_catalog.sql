-- Add image_small_url column and enforce that it must be present when image_url is present.

ALTER TABLE public.milling_tool_catalog
  ADD COLUMN IF NOT EXISTS image_small_url TEXT NULL;

DO $$
BEGIN
  IF NOT EXISTS (
    SELECT 1
    FROM pg_constraint c
    JOIN pg_class t ON t.oid = c.conrelid
    JOIN pg_namespace n ON n.oid = t.relnamespace
    WHERE c.conname = 'milling_tool_catalog_image_url_requires_small_chk'
      AND n.nspname = 'public'
      AND t.relname = 'milling_tool_catalog'
  ) THEN
    ALTER TABLE public.milling_tool_catalog
      ADD CONSTRAINT milling_tool_catalog_image_url_requires_small_chk
      CHECK (image_url IS NULL OR image_small_url IS NOT NULL);
  END IF;
END $$;


