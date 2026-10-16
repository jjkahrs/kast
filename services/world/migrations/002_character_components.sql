-- Lua components with persist = true, per character: {"<Name>": {...}, ...} (README "Lua components and systems").
-- Entries with no current persisted definition are written back unchanged, so a removed definition loses no data.
ALTER TABLE characters ADD COLUMN components JSON NULL;
