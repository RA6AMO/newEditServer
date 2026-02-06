CREATE SEQUENCE IF NOT EXISTS public.global_object_registry_global_id_seq;

CREATE TABLE IF NOT EXISTS public.global_object_registry (
    -- global_id: человекочитаемый глобальный идентификатор
    global_id BIGINT PRIMARY KEY DEFAULT nextval('public.global_object_registry_global_id_seq'),
    -- object_type: тип сущности (например: 'milling_tool')
    object_type TEXT NOT NULL,
    -- object_id: локальный id в таблице
    object_id BIGINT NOT NULL,
    -- created_at: время регистрации
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT global_object_registry_object_uniq UNIQUE (object_type, object_id)
);

-- indexes: ускоряют поиск по global_id
CREATE INDEX IF NOT EXISTS idx_global_object_registry_global_id
    ON public.global_object_registry (global_id);
