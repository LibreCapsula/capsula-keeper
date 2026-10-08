# capsula-keeper

Аппаратный отладочный донгл для Rockchip-платы (RK3308) на базе **ESP32-S3**.
Подключается к компьютеру по USB-C и позволяет по команде из консоли (или от LLM):

1. **перезагружать целевую плату** — «нажимать» кнопку Reset на заданное время;
2. **уводить плату в LOADER или MaskROM** — это два разных механизма на разных
   цепях: LOADER — последовательность Recovery+Reset (для `rkdeveloptool` /
   `upgrade_tool`); MaskROM — короткое замыкание FLASH_D0 через внешний nMOS,
   аппаратно гарантированный путь даже на зависшей плате;
3. **проксировать UART** — стримить логи BootROM/U-Boot/ядра (1500000 8N1) в ПК
   и отправлять данные в консоль платы в обратную сторону.

```
ПК (USB-C, /dev/ttyACM0)          ESP32-S3 SuperMini            Rockchip-плата (RK3308)
┌──────────────┐   нативный USB   ┌──────────────────┐  UART1   ┌─────────────────────┐
│  keeper CLI  │◄────────────────►│  keeper-fw       │◄────────►│ debug UART2 1500000 │
│  (control/)  │                  │  (fw/)           │  1500000 │ TP50 (RX), TP51 (TX)│
└──────────────┘                  └──────────────────┘          └─────────────────────┘

Цепи (подробная карта и схема — docs/wiring.md):
  GPIO4 (RX) ◄────── TP51 (UART2_TX)         ┐ UART-мост, 1500000 8N1
  GPIO5 (TX) ──────► TP50 (UART2_RX)         ┘
  GPIO6 ──330Ω──► Reset (сигнальный контакт кнопки)
  GPIO7 ──330Ω──► Recovery (TP5, цепь ADC_KEY_IN1, домен 1.8 В)
  GPIO8 ──330Ω──► затвор nMOS: сток → TP48 (FLASH_D0), исток → GND,
                  10 кОм затвор→GND (принудительный MaskROM)
  GND ─────────► GND
```

⚠️ Recovery и MaskROM — **две разные функции на двух разных цепях**: GPIO7
«нажимает» кнопку Recovery (TP5), GPIO8 через внешний MOSFET коротит
FLASH_D0 (TP48). Для канала MaskROM обязательны nMOS (2N7002 / AO3400 /
КП505А), резистор ~330 Ом в цепь затвора и 10 кОм затвор→GND — см.
`docs/wiring.md`.

## Структура

- `fw/` — прошивка ESP32-S3 (ESP-IDF v5.5). UART-мост, open-drain «нажиматель»
  кнопок, фреймовый протокол поверх USB CDC.
- `control/` — управляющая программа на C (`keeper`), без зависимостей кроме
  libc: termios + POSIX regex.
- `docs/` — пайка и подключение к плате (`wiring.md`), спецификация протокола
  (`PROTOCOL.md`).
- `e2e_test.py`, `stress_test.py` — тесты CLI без железа (мок-донгл на pty).

Лицензия: MIT (см. `LICENSE`).

## Сборка

### Прошивка

```sh
fw/setup.sh          # один раз: клонирует ESP-IDF v5.5 в .esp-idf и ставит тулчейн
fw/build.sh build    # сборка
fw/build.sh flash    # прошивка по USB (без кнопок — работает USB-Serial-JTAG)
fw/build.sh monitor  # отладочный консоль самой прошивки (уходит в UART0, не в мост)
```

### Control-программа

```sh
make -C control           # бинарник control/build/capsula-keeper
make -C control check     # юнит-тесты парсера протокола
make -C control fuzz      # фаззер парсера под ASan/UBSan
sudo make -C control install    # установить в /usr/local/bin
```

Тесты без железа (мок-донгл на pty): `python3 e2e_test.py` — базовые
сценарии CLI, `python3 stress_test.py` — стресс (обрыв соединения, мусор на
проводе, rapid-fire вызовы, блокировка порта, burst-стрим, ранние логи
`reset --log`).

## Подключение

| ESP32-S3 SuperMini | Целевая плата (capsula / rk3308-evb)          | Примечание                          |
|--------------------|-----------------------------------------------|-------------------------------------|
| GPIO5 (TX)         | UART2 **RX** — тестпоинт TP50                 | крест-накрест, 3.3 В                |
| GPIO4 (RX)         | UART2 **TX** — тестпоинт TP51                 |                                     |
| GPIO6              | Reset: сигнальный контакт кнопки, через 330 Ом | open-drain, «нажатие» = тянет в GND |
| GPIO7              | Recovery (ADC_KEY_IN1, TP5), через 330 Ом      | 1.8 В домен SARADC                  |
| GPIO8              | TP48 (FLASH_D0), через 330 Ом                  | принудительный MaskROM              |
| GND                | GND                                           |                                     |

Подробности, проверка пайки и сценарии — `docs/wiring.md`.
Инструкция для LLM-агентов по управлению платой — `AGENTS.md` в корне репозитория.

Питание: ESP32 — от USB-C ПК; плата питается как обычно (своим БП); соединять
3V3 донгла с платой не нужно.

## Использование

```sh
capsula-keeper ports                  # найти донгл (или --port /dev/ttyACM0, или $KEEPER_PORT)
capsula-keeper ping                   # проверка связи
capsula-keeper status                 # аптайм, скорость, счётчики потерь, состояния кнопок

capsula-keeper reset                  # перезагрузить плату (Reset на 1 с)
capsula-keeper reset --log --max-lines 100   # сброс + сразу первые 100 строк лога
capsula-keeper reset --log 'login:' --timeout 60   # сброс и ждать логин
capsula-keeper reset --ms 3000

capsula-keeper maskrom                # ГАРАНТИРОВАННЫЙ вход в MaskROM (TP48/FLASH_D0)
rkdeveloptool ld              # ... и дальше штатная прошивка Rockchip

capsula-keeper loader                 # через Recovery+Reset (нужен включённый DM_KEY в U-Boot)

capsula-keeper log                    # стрим логов (Ctrl-C)
capsula-keeper log --until 'login:' --timeout 60      # ждать строку — удобно для LLM
capsula-keeper log --capture keeper.log               # параллельно писать в файл

capsula-keeper send ''                # Enter в консоль платы (прервать autoboot U-Boot)
capsula-keeper telnet                   # интерактивная консоль (логи + ввод, Ctrl-] выход)
capsula-keeper send 'version'         # команда в консоль U-Boot/Linux
capsula-keeper press reset --ms 500   # низкоуровневое удержание кнопки
capsula-keeper baud 115200            # если целевая плата не на 1.5 Мбит
```

Рабочий цикл для LLM: `capsula-keeper reset` → `capsula-keeper log --until 'Hit any key'` →
`capsula-keeper send ''` → `capsula-keeper log --until 'login:'` → `capsula-keeper send 'dmesg | tail'`.

## Протокол (ПК ↔ донгл)

ПК → донгл: сырой поток. Строки, начинающиеся с `@`, — команды
(`@PING`, `@RESET <ms>`, `@LOADER [pre rst post]`, `@MASKROM [pre rst post]`,
`@PRESS <btn> [ms]`, `@RELEASE <btn>`, `@BAUD <n>`, `@STAT`, `@VER`); остальное
уходит в UART платы (`@@` — передать литеральный `@`).
Полная спецификация — `docs/PROTOCOL.md`.

Донгл → ПК: бинарные фреймы `[0x5A][type u8][len u16 LE][payload]`, где
type `0x01` = LOG (байты UART платы), `0x02` = CTRL (ответ `OK ...` / `ERR ...`).

## Замечания по RK3308

- Debug UART: **1500000 8N1**, уровень 3.3 В на отладочных платах — сверьтесь
  со схемой своей платы (если VCCIO 1.8 В, на RX ESP32 нужен делитель).
- Вход в LOADER: Recovery должен быть зажат в момент, когда BootROM стартует
  после снятия Reset; дефолтные тайминги прошивки (200/1000/3000 мс) это
  покрывают, их можно менять опциями `capsula-keeper loader`.
- Прошивка через `rkdeveloptool` идёт по USB OTG самой платы — конфликтов с
  UART-мостом нет.
