CREATE TABLE IF NOT EXISTS public.users (
    id            BIGSERIAL PRIMARY KEY,
    username      VARCHAR(255)   NOT NULL UNIQUE,
    password_hash TEXT           NOT NULL,
    created_at    TIMESTAMPTZ    NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ    NOT NULL DEFAULT now(),
    last_login_at TIMESTAMPTZ    NULL
);