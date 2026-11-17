#!/bin/sh
# uso: winrtmidi.sh install|uninstall [WINEPREFIX]
# Feche o app antes. Atualizações de prefixo do Proton podem desfazer o registro: rode install de novo.
# WINE/WINESERVER: binários a usar. PROTON_FILES=<proton>/files usa o wine de um Proton.
set -e
D="$(cd "$(dirname "$0")" && pwd)"
PFX="${2:-${WINEPREFIX:-$HOME/.wine}}"
if [ -n "$PROTON_FILES" ]; then
  WINE="${WINE:-$PROTON_FILES/bin/wine}"; WINESERVER="${WINESERVER:-$PROTON_FILES/bin/wineserver}"
fi
WINE="${WINE:-wine}"; WINESERVER="${WINESERVER:-wineserver}"
export WINEPREFIX="$PFX" WINEDEBUG=-all
case "$1" in
  install)
    [ -f "$D/winrtmidi.dll" ] || { echo "winrtmidi.dll não encontrada (rode make)"; exit 1; }
    cp "$D/winrtmidi.dll" "$PFX/drive_c/windows/system32/winrtmidi.dll"
    "$WINE" regedit /S "$D/install.reg"
    "$WINESERVER" -w
    echo "instalado em $PFX" ;;
  uninstall)
    "$WINE" regedit /S "$D/uninstall.reg"
    "$WINESERVER" -w
    rm -f "$PFX/drive_c/windows/system32/winrtmidi.dll"
    echo "removido de $PFX" ;;
  *) echo "uso: $0 install|uninstall [WINEPREFIX]"; exit 1 ;;
esac
