#!/bin/bash
# Установка ygg-control на Linux:
#   sudo ./install.sh            - команда ygg-control в системе
#   sudo ./install.sh --remove   - убрать команду (файлы в папке остаются)
# Панель работает прямо из этой папки: там же её адресная книга (addressbook.txt).
set -e
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
if [ "$(id -u)" -ne 0 ]; then echo "Нужны права root: sudo $0 $*"; exit 1; fi
LINK=/usr/local/bin/ygg-control
if [ "$1" = "--remove" ]; then
    if [ -L "$LINK" ]; then rm -f "$LINK"; echo "Команда ygg-control убрана."; else echo "Команды ygg-control и так нет."; fi
    exit 0
fi
[ -x "$DIR/ygg-control" ] || chmod +x "$DIR/ygg-control" 2>/dev/null || true
if [ ! -x "$DIR/ygg-control" ] || ! "$DIR/ygg-control" --selftest >/dev/null 2>&1; then
    echo "ygg-control не запускается. Собери его: ./build.sh (нужны g++ и libssl-dev)"
    exit 1
fi
command -v yggdrasilctl >/dev/null || echo "Внимание: не найден yggdrasilctl - сначала установи Yggdrasil."
ln -sfn "$DIR/ygg-control" "$LINK"
echo "Готово: ygg-control $("$DIR/ygg-control" --selftest). Открыть панель: ygg-control"
command -v ygg-watchdog >/dev/null || [ -f /etc/yozmor/watchdog_path ] || \
    echo "Сторож пиров ygg-watchdog не найден - панель работает и без него (github.com/Yozmor/ygg-watchdog)."
