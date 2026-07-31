# lr_core

Standalone-система рангов (Levels Ranks) для **CS2**, написанная с нуля на C++ (MetaMod + Source2SDK).

Аналог по логике Pisex `cs2-lvl_ranks`, но без его зависимостей (`utils`, `menus`, `cookies`, `sql_mm` не требуются) и с исправленным багом двойного создания колонки `online`, из-за которого исходный плагин падал при рестарте.

## Возможности

- Три режима подсчёта опыта (как у Pisex):
  - `0` — накопительная (очки за действия, уровни по порогам);
  - `1` — рейтинговая расширенная (аналог HLstatsX);
  - `2` — рейтинговая простая (аналог RankMe).

- Бонусы за серии убийств (DoubleKill … GodLike), учёт бомбы, заложников, MVP.

- Отображение уровня в TAB (скорборд) через `m_iCompetitiveRank*` + `ServerRankRevealAll` — настраивается в `configs/tab.ini`.

- MySQL (async, отдельный поток + реконнект). Схема совместима с Pisex/LR-web, добавлены колонки:
  - `steamid64`
  - `reset_cooldown`

- Чат-команды:
  - `!rank`
  - `!lvl`
  - `!top`
  - `!toptime`
  - `!session`
  - `!resetstats`

- Серверные консольные команды для интеграции (C#-плагины / сайт):
  - `lr_exp <steamid> <exp>` (работает и для оффлайн-игроков);
  - `lr_reset <steamid>`;
  - `lr_reload`;
  - `lr_status`.

- Публичный MetaMod-интерфейс `LRCoreApi001` (`public/lr_api.h`) для будущих C++-модулей.

## Технические детали

- Захват `IGameEventManager2` и `CGameEntitySystem` — через DVP-хук по vtable, найденной RTTI-сканом ELF (`src/vtable_finder.cpp`), без gamedata-сигнатур.

- Доступ к полям сущностей — рантайм-резолв оффсетов через `ISchemaSystem` (`src/schema.cpp`) + корректный `NetworkStateChanged`.

- Чат — юзермесседж `TextMsg` (`FindNetworkMessagePartial` + `PostEventAbstract`).

## Сборка (Linux, x86_64)

Нужны:

- clang
- ambuild
- hl2sdk-cs2
- metamod-source
- hl2sdk-manifests
- статическая `mariadb-connector-c`

Пути задаются в `build_linux.sh` (собирается в Docker, образ `ubuntu:20.04` + `clang-10` + `ambuild` + `mariadb-connector-c` в `/opt/mariadb`).

```bash
bash build_linux.sh

# результат:
build_linux/package/addons/...
```

## Установка

1. Скопировать `addons/lr_core/` и `addons/metamod/lr_core.vdf` из собранного пакета в `game/csgo/`.

2. Выполнить (в `game/csgo/`):

```bash
cp addons/lr_core/configs/database.ini.example addons/lr_core/configs/database.ini
```

и вписать доступы к своей MySQL-базе. Поддерживаются charset'ы
`utf8mb4` / `utf8` / `latin1` / `ascii` / `binary` — на остальных плагин
откажется стартовать, потому что побайтовое экранирование строк на них
некорректно.

3. При необходимости отредактировать:
   - `settings.ini` ( `lr_table`, режим статистики, пороги уровней );
   - `tab.ini` (иконки в TAB).

4. Перезапустить сервер.

### Кастомные иконки рангов в TAB

Клиент рисует `panorama/images/icons/skillgroups/skillgroup{N}.svg` из своих
файлов, поэтому свои бейджи надо доставить ему воркшоп-аддоном — FastDL в CS2
не работает. Исходники лежат в `ranks/`, соответствие «файл → ID → уровень»
и требования к SVG описаны в [ranks/README.md](ranks/README.md).

Раздачу аддона клиентам делает [MultiAddonManager](https://github.com/Source2ZE/MultiAddonManager)
(`mm_extra_addons "<workshop_id>"`), сам `lr_core` только пишет число в
`m_iCompetitiveRanking`. Проверка подобранных ID — `lr_tab_test <steamid64> <value>`.

Если не хотите заставлять игроков что-то качать — поставьте `tab_type 11`
и мапьте уровни на ванильные 1..18.

Проверка:

```text
meta list
```

Должно отображаться:

```text
[LR] Core
```

Проверка состояния:

```text
lr_status
```

Ожидаемый вывод:

```text
ready=1
db_connected=1
```

## Структура

```text
src/             исходники плагина
public/          публичный API (lr_api.h) для сторонних C++-модулей
configs/         settings.ini, tab.ini, database.ini.example
translations/    фразы (ru/en) + названия званий
ranks/           SVG-бейджи для воркшоп-аддона (в пакет плагина не входят)

AMBuild*
configure.py
PackageScript
build_linux.sh   сборка
```

## Лицензия

GPL (использует SourceHook / MetaMod).

# ranks
