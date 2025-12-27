CREATE TABLE IF NOT EXISTS public.milling_tool_catalog (
    -- id: первичный ключ записи (идентификатор модели/типоразмера фрезы)
    id BIGSERIAL PRIMARY KEY,

    -- name: человекочитаемое наименование инструмента (например: "Фреза концевая сферическая D12 R6")
    name TEXT NOT NULL,

    -- reference ids: ссылки на локальные справочники (FOREIGN KEY можно добавить позже)
    -- tool_type_id: тип фрезы (концевая/сферическая/черновая/фасочная и т.п.)
    tool_type_id BIGINT NULL,
    -- brand_id: производитель/бренд
    brand_id BIGINT NULL,
    -- shank_type_id: тип хвостовика (цилиндр/Weldon/конус и т.п.)
    shank_type_id BIGINT NULL,

    -- coolant_through: признак подачи СОЖ через инструмент (true/false)
    coolant_through BOOLEAN NULL,

    -- shank_diameter_mm: диаметр хвостовика, мм
    shank_diameter_mm NUMERIC(10, 3) NULL,
    -- overall_length_mm: общая длина инструмента, мм
    overall_length_mm NUMERIC(10, 3) NULL,
    -- cutting_diameter_mm: режущий диаметр (D), мм
    cutting_diameter_mm NUMERIC(10, 3) NULL,
    -- flute_length_mm: длина режущей части (длина канавок), мм
    flute_length_mm NUMERIC(10, 3) NULL,
    -- corner_radius_mm: радиус на углу (R), мм; 0 = острый угол
    corner_radius_mm NUMERIC(10, 3) NULL DEFAULT 0,
    -- taper_angle_deg: угол конусности, градусы (для конических фрез)
    taper_angle_deg NUMERIC(10, 3) NULL,
    -- flutes_count: количество зубьев/канавок (Z)
    flutes_count INTEGER NULL,

    -- qty_on_stock: количество на складе (остаток), шт.
    qty_on_stock INTEGER NULL,

    -- qty_in_use: количество в пользовании/в работе, шт.
    qty_in_use INTEGER NULL,

    -- notes: комментарий/примечания
    notes TEXT NULL,

    -- image_url: URL или путь к изображению (сами картинки лучше хранить в файловом хранилище/объектном хранилище)
    image_exists BOOLEAN NULL DEFAULT FALSE,

    -- constraints: количества не могут быть отрицательными (NULL допустим для MVP)
    CONSTRAINT milling_tool_catalog_qty_on_stock_nonnegative_chk
      CHECK (qty_on_stock IS NULL OR qty_on_stock >= 0),
    CONSTRAINT milling_tool_catalog_qty_in_use_nonnegative_chk
      CHECK (qty_in_use IS NULL OR qty_in_use >= 0)
);

-- indexes: ускоряют поиск/фильтрацию (ценой небольшого замедления вставки/обновления)
CREATE INDEX IF NOT EXISTS idx_milling_tool_catalog_name
    ON public.milling_tool_catalog (name);

CREATE INDEX IF NOT EXISTS idx_milling_tool_catalog_tool_type_id
    ON public.milling_tool_catalog (tool_type_id);

CREATE INDEX IF NOT EXISTS idx_milling_tool_catalog_brand_id
    ON public.milling_tool_catalog (brand_id);

-- Таблица изображений для инструментов (1:1 связь с milling_tool_catalog)
-- Хранит ссылки на MinIO для большого и маленького изображений, а также метаданные для ImageWithLink
CREATE TABLE IF NOT EXISTS public.milling_tool_images (
    -- tool_id: ссылка на инструмент (PRIMARY KEY, т.к. связь 1:1)
    tool_id BIGINT PRIMARY KEY REFERENCES public.milling_tool_catalog(id) ON DELETE CASCADE,

    -- big: большое изображение (оригинал)
    big_bucket TEXT NULL,
    big_object_key TEXT NULL,
    big_mime_type TEXT NULL,
    big_size_bytes BIGINT NULL,

    -- small: маленькое изображение (превью, копия большого)
    small_bucket TEXT NULL,
    small_object_key TEXT NULL,
    small_mime_type TEXT NULL,
    small_size_bytes BIGINT NULL,

    -- метаданные для ImageWithLink (опционально)
    link_name TEXT NULL,
    link_url TEXT NULL,

    -- временные метки
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
