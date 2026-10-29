#!/bin/bash
# Сборка ygg-control для Linux. Нужны g++ и заголовки OpenSSL:
#   sudo apt install g++ libssl-dev
# Готовая программа - ./ygg-control рядом с этим файлом.
set -e
cd "$(dirname "$(readlink -f "$0")")"
if ! command -v g++ >/dev/null; then echo "Нет g++: sudo apt install g++ libssl-dev"; exit 1; fi
if [ ! -f /usr/include/openssl/ssl.h ]; then echo "Нет заголовков OpenSSL: sudo apt install libssl-dev"; exit 1; fi
echo "Собираю ygg-control..."
g++ -std=c++17 -O2 -Wall -Wextra -Isrc -o ygg-control.build src/panel/main.cpp \
    -static-libstdc++ -static-libgcc -lssl -lcrypto -pthread
mv -f ygg-control.build ygg-control
echo "Готово: $(pwd)/ygg-control ($(./ygg-control --selftest))"
