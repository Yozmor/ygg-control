#!/bin/bash
# Установка ygg-control на Linux:
#   sudo ./install.sh            - команды yggdrasil и ygg-control открывают панель
#   sudo ./install.sh --remove   - убрать команды (файлы в папке остаются)
# Панель работает прямо из этой папки: там же её адресная книга (addressbook.txt).
# «yggdrasil» с аргументами (-genconf, -version, -useconffile ...) панель передаёт
# настоящему Yggdrasil - он и служба работают как раньше.
set -e
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
if [ "$(id -u)" -ne 0 ]; then echo "Нужны права root: sudo $0 $*"; exit 1; fi
SELF="$DIR/ygg-control"
LINKS="/usr/local/bin/ygg-control /usr/local/bin/yggdrasil"
ours() { [ -L "$1" ] && [ "$(readlink -f "$1")" = "$(readlink -f "$SELF")" ]; }
if [ "$1" = "--remove" ]; then
    for l in $LINKS; do if ours "$l"; then rm -f "$l"; echo "убрана команда $(basename "$l")"; fi; done
    hash -r 2>/dev/null || true
    exit 0
fi
[ -x "$SELF" ] || chmod +x "$SELF" 2>/dev/null || true
if [ ! -x "$SELF" ] || ! "$SELF" --selftest >/dev/null 2>&1; then
    echo "ygg-control не запускается. Собери его: ./build.sh (нужны g++ и libssl-dev)"
    exit 1
fi
command -v yggdrasilctl >/dev/null || echo "Внимание: не найден yggdrasilctl - сначала установи Yggdrasil."
ln -sfn "$SELF" /usr/local/bin/ygg-control
Y=/usr/local/bin/yggdrasil
CMD="yggdrasil (или ygg-control)"
if [ -e "$Y" ] && ! ours "$Y"; then
    echo "В $Y лежит сам Yggdrasil - его не трогаю. Панель открывается командой ygg-control."
    CMD="ygg-control"
else
    ln -sfn "$SELF" "$Y"
    # настоящий yggdrasil должен остаться доступен: панель передаёт ему команды с аргументами
    if ! "$Y" -version >/dev/null 2>&1; then
        echo "Внимание: настоящий yggdrasil не найден - команды вида «yggdrasil -genconf» работать не будут."
    fi
fi
hash -r 2>/dev/null || true
echo "Готово: ygg-control $("$SELF" --selftest). Открыть панель: $CMD"
[ "$CMD" = "ygg-control" ] || echo "Если в этом окне терминала yggdrasil всё ещё ведёт себя по-старому - выполни: hash -r"
command -v ygg-watchdog >/dev/null || [ -f /etc/yozmor/watchdog_path ] || \
    echo "Сторож пиров ygg-watchdog не найден - панель работает и без него (github.com/Yozmor/ygg-watchdog)."
