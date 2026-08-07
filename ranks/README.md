# Кастомные иконки рангов (workshop addon)

Клиент CS2 рисует в TAB файл:

```text
panorama/images/icons/skillgroups/skillgroup{N}.vsvg_c
```

из **смонтированного** workshop-аддона. FastDL в CS2 нет.

## Соответствие ID → уровень

| Файл | `m_iCompetitiveRanking` | Уровень LR |
|------|-------------------------|------------|
| `skillgroup50.svg` … `skillgroup69.svg` | 50 … 69 | 1 … 20 |
| `skillgroup70.svg` | 70 | 21+ (AboveMax) |

Эти же числа должны стоять в `configs/tab.ini` → `Levels` / `AboveMax`.

## Как правильно собрать и залить аддон

Ошибка `Unable to parse SVG '...skillgroupN.vsvg'` / чёрные силуэты в Asset Browser
значат: SVG несовместим с Panorama (Inkscape-фичи) или скомпилированный `.vsvg` битый.

### Подготовка файлов

1. Установить **Counter-Strike 2 Workshop Tools**.
2. Создать addon (латиница, без пробелов), например `perfecteam_ranks`.
3. **Удалить** старые битые файлы в обоих местах (иначе Tools не пересоберёт):

```text
…/content/csgo_addons/perfecteam_ranks/panorama/images/icons/skillgroups/*
…/game/csgo_addons/perfecteam_ranks/panorama/images/icons/skillgroups/*
```

4. Положить **новые** `.svg` **только** сюда:

```text
…/content/csgo_addons/perfecteam_ranks/panorama/images/icons/skillgroups/skillgroup50.svg
…
…/skillgroup70.svg
```

**Не** кладите `.svg` в `game/csgo_addons/` — туда Tools сами пишут `.vsvg_c`.

5. Запустить Workshop Tools → ваш addon → Asset Browser.
6. Справа сверху **Asset Types** → напротив **Vector Graphic (.vsvg, .svg)** нажать **Only**.
7. Дождаться превью: должны быть **цветные** бейджи с цифрами.
   Если снова чёрные щиты и `(No Info)` — SVG всё ещё несовместим, не публикуйте.
8. Tools → Counter-Strike 2 Workshop Manager → **Re-Upload** (не New).
9. На сервере:

```text
mm_extra_addons "3772685382"
```

и `mm_download_addon 3772685382` + смена карты.

### Формат SVG для CS2 / Panorama

**Сжимать в `32×13` не нужно** — сохраняем исходный `viewBox` оригинала.

Но сырой Inkscape-экспорт Panorama **не парсит** (`Unable to parse SVG`). Нужен
упрощённый SVG:

- абсолютные команды `M/C/L/Z` (без relative `m/c/l`);
- transform’ы запечены в координаты;
- градиенты → сплошные цвета;
- без `mm`, `xlink:href` на градиентах, `mix-blend-mode`;
- у stroke-only контуров нет чёрной заливки по умолчанию (они пропускаются).

Оригиналы Inkscape — в `ranks/_backup_before_32x13/`. Пересобрать рабочие копии:

```bash
pip install svgelements
python ranks/prepare_for_cs2.py
```

Проверка: в Asset Browser бейдж должен быть **цветным щитом с цифрой**.
Перед копированием удали кэш в `game/csgo_addons/<addon>/panorama/images/icons/skillgroups/`.
