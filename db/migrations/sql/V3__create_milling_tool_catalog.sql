CREATE TABLE IF NOT EXISTS public.milling_tool_catalog (
    -- id: первичный ключ записи (идентификатор модели/типоразмера фрезы)
    id BIGSERIAL PRIMARY KEY,
    -- name: человекочитаемое наименование инструмента (например: "Фреза концевая сферическая D12 R6")
    name TEXT NULL,

    -- reference ids: ссылки на локальные справочники (FOREIGN KEY можно добавить позже)
    -- child_type_id: идентификатор дочерней "логической" таблицы
    child_type_id BIGINT NULL,
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

    -- image_exists: ID изображения (строки) в public.milling_tool_images, связанного со слотом 'image_exists'
    -- NULL = изображения нет
    image_exists BIGINT NULL,

    -- is_deleted: признак soft delete (true = скрыта)
    is_deleted BOOLEAN NOT NULL DEFAULT false,

    -- deleted_at: время soft delete (NULL = не удалена)
    deleted_at TIMESTAMPTZ NULL,

    -- constraints: количества не могут быть отрицательными (NULL допустим для MVP)
    CONSTRAINT milling_tool_catalog_qty_on_stock_nonnegative_chk
      CHECK (qty_on_stock IS NULL OR qty_on_stock >= 0),
    CONSTRAINT milling_tool_catalog_qty_in_use_nonnegative_chk
      CHECK (qty_in_use IS NULL OR qty_in_use >= 0)
);

-- indexes: ускоряют поиск/фильтрацию (ценой небольшого замедления вставки/обновления)
CREATE INDEX IF NOT EXISTS idx_milling_tool_catalog_name
    ON public.milling_tool_catalog (name);

CREATE INDEX IF NOT EXISTS idx_milling_tool_catalog_child_type_id
    ON public.milling_tool_catalog (child_type_id);

CREATE INDEX IF NOT EXISTS idx_milling_tool_catalog_brand_id
    ON public.milling_tool_catalog (brand_id);

CREATE INDEX IF NOT EXISTS idx_milling_tool_catalog_soft_delete
    ON public.milling_tool_catalog (is_deleted, deleted_at);

-- Таблица изображений для инструментов (1:N связь с milling_tool_catalog)
-- Хранит ссылки на MinIO для большого и маленького изображений, а также метаданные для ImageWithLink
-- 1:N связь: один инструмент -> много изображений (по слотам image_*)
CREATE TABLE IF NOT EXISTS public.milling_tool_images (
    -- id: первичный ключ изображения
    id BIGSERIAL PRIMARY KEY,

    -- tool_id: ссылка на инструмент (1 инструмент -> N изображений)
    tool_id BIGINT NOT NULL REFERENCES public.milling_tool_catalog(id) ON DELETE CASCADE,

    -- slot: имя колонки в milling_tool_catalog, к которой относится изображение (например: 'image_exists')
    slot TEXT NOT NULL,

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

-- 1 изображение на 1 image_* ячейку (по слоту) для конкретного инструмента
ALTER TABLE public.milling_tool_images
  ADD CONSTRAINT milling_tool_images_tool_slot_uniq UNIQUE (tool_id, slot);

-- FK: image_exists -> milling_tool_images.id
ALTER TABLE public.milling_tool_catalog
  ADD CONSTRAINT milling_tool_catalog_image_exists_fk
  FOREIGN KEY (image_exists)
  REFERENCES public.milling_tool_images(id)
  ON DELETE SET NULL;

-- Индексы для частых выборок
CREATE INDEX IF NOT EXISTS idx_milling_tool_images_tool_id
    ON public.milling_tool_images (tool_id);

CREATE INDEX IF NOT EXISTS idx_milling_tool_images_tool_id_slot
    ON public.milling_tool_images (tool_id, slot);

-- Sync-триггер: изменения в milling_tool_images синхронизируют соответствующую image_* колонку в milling_tool_catalog
CREATE OR REPLACE FUNCTION public.sync_milling_tool_images_to_catalog()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
BEGIN
  IF (TG_OP = 'INSERT') THEN
    EXECUTE format('UPDATE public.milling_tool_catalog SET %I = $1 WHERE id = $2', NEW.slot)
      USING NEW.id, NEW.tool_id;
    RETURN NEW;
  END IF;

  IF (TG_OP = 'UPDATE') THEN
    -- если картинку переместили на другой tool/slot, сначала очистим старую ячейку (только если она указывала на OLD.id)
    IF (NEW.tool_id <> OLD.tool_id) OR (NEW.slot <> OLD.slot) THEN
      EXECUTE format('UPDATE public.milling_tool_catalog SET %I = NULL WHERE id = $1 AND %I = $2', OLD.slot, OLD.slot)
        USING OLD.tool_id, OLD.id;
    END IF;

    -- затем выставим новую ячейку
    EXECUTE format('UPDATE public.milling_tool_catalog SET %I = $1 WHERE id = $2', NEW.slot)
      USING NEW.id, NEW.tool_id;

    RETURN NEW;
  END IF;

  IF (TG_OP = 'DELETE') THEN
    EXECUTE format('UPDATE public.milling_tool_catalog SET %I = NULL WHERE id = $1 AND %I = $2', OLD.slot, OLD.slot)
      USING OLD.tool_id, OLD.id;
    RETURN OLD;
  END IF;

  RETURN NULL;
END;
$$;

DROP TRIGGER IF EXISTS trg_sync_milling_tool_images_to_catalog ON public.milling_tool_images;

CREATE TRIGGER trg_sync_milling_tool_images_to_catalog
AFTER INSERT OR UPDATE OR DELETE ON public.milling_tool_images
FOR EACH ROW
EXECUTE FUNCTION public.sync_milling_tool_images_to_catalog();
