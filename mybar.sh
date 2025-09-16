#!/bin/sh
# mybar.sh - barra simples para MyWM usando lemonbar
# Lê eventos JSON do socket do WM e mostra workspaces, janela focada e estado da bar

SOCK="${XDG_RUNTIME_DIR:-/tmp}/mywm.sock"

if [ ! -S "$SOCK" ]; then
  echo "mybar: socket do WM não encontrado em $SOCK" >&2
  exit 1
fi

# cores
FG="#ffffff"
BG="#222222"
ACTIVE="#88ccff"
OCCUPIED="#aaaaaa"
EMPTY="#444444"

# estado inicial
workspaces="1 2 3 4 5"
active_ws=1
occupied_ws="1"
title=""

# função de renderização
render() {
  out=""
  for ws in $workspaces; do
    if [ "$ws" = "$active_ws" ]; then
      out="$out %{B$ACTIVE} $ws %{B-}"
    elif echo "$occupied_ws" | grep -qw "$ws"; then
      out="$out %{B$OCCUPIED} $ws %{B-}"
    else
      out="$out %{B$EMPTY} $ws %{B-}"
    fi
  done
  echo "%{l}$out %{c}$title %{r}MyWM"
}

# loop: escuta eventos JSON do WM
socat - UNIX-CONNECT:"$SOCK" | while read -r line; do
  case "$line" in
    *\"event\":\"workspace\"*)
      active_ws=$(echo "$line" | sed -n 's/.*"active":\([0-9]\+\).*/\1/p')
      occupied_ws=$(echo "$line" | sed -n 's/.*"occupied":\[\([^]]*\)\].*/\1/p' | tr -d ,)
      ;;
    *\"event\":\"focus\"*)
      title=$(echo "$line" | sed -n 's/.*"title":"\([^"]*\)".*/\1/p')
      ;;
    *\"event\":\"bar-toggle\"*"false"*)
      echo "" > /tmp/mybar-hidden
      continue
      ;;
    *\"event\":\"bar-toggle\"*"true"*)
      rm -f /tmp/mybar-hidden
      ;;
  esac
  [ -e /tmp/mybar-hidden ] || render
done | lemonbar -g x24 -B "$BG" -F "$FG" -p -d
