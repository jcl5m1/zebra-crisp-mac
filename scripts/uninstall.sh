#!/bin/bash
# Removes zebra-crisp-mac and switches its printers back to Apple's stock
# "Zebra ZPL Label Printer" driver, keeping each printer's saved defaults.
#
# Usage: sudo /Library/Printers/ZebraCrisp/uninstall.sh

if [ "$(id -u)" -ne 0 ]; then
  echo "Please run with sudo: sudo $0" >&2
  exit 1
fi

OPTIONS="PageSize Resolution zeMediaTracking MediaType Darkness zePrintRate zeLabelTop zePrintMode zeTearOffPosition zeErrorReprint"

for ppd in /etc/cups/ppd/*.ppd; do
  [ -f "$ppd" ] || continue
  grep -q '^\*NickName: "Zebra ZPL Label Printer Crisp"' "$ppd" || continue
  queue="$(basename "$ppd" .ppd)"

  # Saved defaults live on the CUPS server, not in the queue's PPD copy.
  current="$(lpoptions -p "$queue" | tr ' ' '\n')"
  defaults=()
  for opt in $OPTIONS; do
    value="$(printf '%s\n' "$current" | sed -n "s/^$opt=//p" | head -1)"
    [ -n "$value" ] && defaults+=(-o "$opt-default=$value")
  done

  echo "Switching printer $queue back to the stock Zebra ZPL driver"
  lpadmin -p "$queue" -m drv:///sample.drv/zebra.ppd
  # Defaults must be a separate call; lpadmin drops them when given with -m.
  [ ${#defaults[@]} -gt 0 ] && lpadmin -p "$queue" "${defaults[@]}"
done

rm -f /Library/Printers/PPDs/Contents/Resources/ZebraCrisp.ppd
rm -f "/Library/PDF Services/Preview Zebra Output"
rm -rf /Library/Printers/ZebraCrisp
pkgutil --forget com.github.jcl5m1.zebra-crisp-mac >/dev/null 2>&1

echo "zebra-crisp-mac removed."
