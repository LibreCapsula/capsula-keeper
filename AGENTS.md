# AGENTS.md — отладка платы через capsula-keeper

Инструкция для LLM-агентов, работающих с платой на Rockchip SoC (проверено
на RK3308), к которой подключён аппаратный донгл capsula-keeper (ESP32-S3):
UART-мост, электронные «кнопки» Reset / Recovery и канал принудительного
входа в MaskROM.

- CLI: `capsula-keeper` (сборка и установка: `make -C control &&
  sudo make -C control install`; бинарник обычно уже стоит в
  `/usr/local/bin/capsula-keeper`).
- Если нет прав на `/dev/ttyACM0` — работайте через `sudo capsula-keeper ...`.
- Все команды идемпотентны и безопасны, кроме явно отмеченных ниже.
- Схема подключения донгла к плате — `docs/wiring.md`; формат протокола —
  `docs/PROTOCOL.md`.

## Быстрая диагностика

```sh
capsula-keeper ping      # донгл жив? -> "OK pong" (exit 0)
capsula-keeper status    # up_s, baud, drops, состояния линий
capsula-keeper usb       # какие Rockchip (2207:*) устройства на USB
capsula-keeper ports     # на каком порту донгл
```

В `status` поля `rst=`, `rec=`, `msk=` — состояния линий Reset / Recovery /
MaskROM. **Норма — `release` у всех трёх.** Если что-то в `press` без вашей
команды — верните: `capsula-keeper release <reset|recovery|maskrom>`.

## Типовые сценарии

### 1. Перезагрузить плату

```sh
capsula-keeper reset                 # ~1 с удержания Reset, exit 0 = "OK reset done"
```

#### 1а. Сброс и сразу читать логи (одной командой)

`reset --log` после сброса не закрывает порт, а продолжает стримить UART —
логи самого начала загрузки не теряются.

```sh
capsula-keeper reset --log --max-lines 100        # сброс + первые 100 строк лога
capsula-keeper reset --log 'login:' --timeout 60  # сброс + ждать строку (exit 0)
capsula-keeper reset --log 'Hit any key' --timeout 5 --max-lines 200
```

- Значение `--log` (если задано) = паттерн `--until`: ждать строку после
  рестарта. Без значения — просто стрим.
- `--max-lines` / `--timeout` пробрасываются в стрим как есть; срабатывает
  то, что наступит первым (лимит/строка → exit 0, таймаут → exit 2).
- Комбинация для агента по умолчанию: `reset --log --max-lines 100` —
  гарантирует и возврат управления, и снимок начала загрузки.

### 2. Посмотреть логи (UART-консоль платы)

```sh
capsula-keeper log --max-lines 100                    # первые 100 строк и стоп
capsula-keeper log --until 'login:' --timeout 60      # ждать строку (exit 0)
capsula-keeper log --until 'Hit any key' --timeout 5  # поймать autoboot U-Boot
capsula-keeper log --capture board.log                # параллельно писать в файл
```

- exit 0 — условие/лимит достигнуты; exit 2 — сработал `--timeout`.
- Всегда ставьте `--timeout` или `--max-lines` — без них стрим бесконечный.
- Если `log` молчит, а в `status` `rx_total=0` — UART-провода платы не
  подключены (см. `docs/wiring.md`), логов физически нет. Не ретраить
  вслепую — проверьте `rx_total` после сброса (`reset` → `log`).

### 3. Ввести команды в консоль платы (когда она жива)

```sh
capsula-keeper send ''             # Enter — прервать autoboot U-Boot
capsula-keeper send 'version'      # команда в U-Boot
capsula-keeper log --max-lines 50  # прочитать ответ
```

#### 3а. Интерактивная консоль (telnet-режим)

```sh
capsula-keeper telnet                     # логи на экран, клавиатура — в плату
capsula-keeper telnet --timeout 30        # авто-выход через 30 с (exit 2)
echo 'dmesg | tail' | capsula-keeper telnet --timeout 5   # скриптовый ввод
```

- Выход — **Ctrl-]** (или Ctrl-C, если stdin не терминал — по EOF).
- Всё, что вы нажимаете, уходит в UART платы сразу (raw-режим): можно
  прерывать autoboot, вводить команды U-Boot/Linux в реальном времени.
- `@` экранируется автоматически (донгл не перехватит его как свою команду).
- Пока открыт telnet, порт занят — другие команды capsula-keeper ждут.

### 4. Ввести плату в MaskROM (гарантированно, для прошивки rkdeveloptool)

```sh
capsula-keeper maskrom
# exit 0 + "confirmed: target in download mode (2207:330e ...)" — можно шить
sudo rkdeveloptool ld        # покажет "Maskrom"
# ... штатная прошивка через rkdeveloptool ...
capsula-keeper reset         # вывести плату из MaskROM в нормальную загрузку
```

- exit 4 = MaskROM не появился на USB за 6 с — CLI сам напечатает чеклист
  (OTG-кабель платы в ПК, сток MOSFET на TP48, `--post-ms 3000`).
- Это аппаратно гарантированный путь (короткое замыкание FLASH_D0), работает
  даже с зависшей платой.
- `capsula-keeper loader` (через кнопку Recovery) работает только если в
  U-Boot целевой платы включено чтение клавиши (CONFIG_DM_KEY и т.п.) —
  проверьте свою сборку, прежде чем использовать.

### 5. Проверить, в каком состоянии плата на USB

```sh
capsula-keeper usb
# 2207:330e  MaskROM (RK3308 BootROM)  — ждёт прошивку
# прочие 2207:* (например 0006)        — это USB-gadget Linux, НЕ download-режим
```

## Рабочий цикл агента

Типичная сессия отладки: `reset --log --max-lines 100` (снимок начала
загрузки) → `log --until 'Hit any key' --timeout 5` (поймать autoboot) →
`send ''` (прервать autoboot) → `send 'команда'` + `log --max-lines 50`
(выполнить и прочитать) — либо `telnet` для интерактива.

## Коды выхода

| Код | Значение |
|-----|----------|
| 0 | успех (для `maskrom`/`loader` — подтверждено по USB) |
| 1 | ошибка (донгл не найден, USB-проверка не пройдена для maskrom и т.п.) |
| 2 | истёк `--timeout` у `log` / `reset --log` / `telnet`, плохие аргументы |

## Правила безопасности

1. **Никогда не оставляйте линии в `press`.** После ручных экспериментов —
   `capsula-keeper status`, все три в `release`. Зажатый `msk` превращает
   каждый старт платы в MaskROM и ломает доступ к NAND, зажатый `rst` не даёт
   плате стартовать.
2. `press maskrom` нельзя держать под работающим Linux — это ломает доступ к
   NAND. Только как последовательность `maskrom` (она сама отпускает).
3. Не перезагружайте плату донглом во время активной прошивки
   rkdeveloptool — сначала дождитесь завершения.
4. Питание платы — отдельное; донгл его не обеспечивает.

## Если что-то не так

| Симптом | Действие |
|---|---|
| `ping`: not found | донгл отцеплен: `lsusb \| grep 303a`, переподключите, `--port /dev/ttyACM0` |
| `ping`: timeout | донгл есть, но молчит: переподключите USB, затем `ping` снова |
| `log` пустой | `rx_total=0` в `status` → UART платы не подключён (аппаратно) |
| `reset --log` пустой после сброса | то же самое; либо слишком короткий `--timeout` для данной прошивки |
| `maskrom` exit 4 | чеклист в выводе; чаще всего OTG-кабель или провод на TP48 |
| плата «не стартует» | проверьте `status`: `msk=press`? → `release maskrom`, затем `reset` |
| `ERR busy` | предыдущая последовательность ещё идёт — подождите пару секунд |
