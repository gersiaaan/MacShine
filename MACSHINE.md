# MacShine – Sunshine mit virtuellen Displays pro Client für macOS

Sunshine (LizardByte, Commit `7974a40`) plus virtuelle Displays nach Apollo-Prinzip.
Display-Logik angelehnt an OpenDisplay (peetzweg/opendisplay). Beide GPL-3.0, dieser Patch ebenfalls.

## Was es macht
- Beim Stream-Start entsteht ein virtuelles Display in **Auflösung und Bildrate des Clients** (Moonlight-Einstellung).
- Optional **Retina/HiDPI** (Standard an): Client 2732×2048 → Desktop 1366×1024 pt, gestreamt mit 2732×2048 px, scharfer Text.
- Optional **Hauptdisplay** (Standard an): Menüleiste und Dock wandern auf das virtuelle Display → voller Desktop auf dem Client.
- Optional **Host-Displays aus** (Standard aus): Während des Streams werden die eigenen Displays des Macs abgeschaltet, der Desktop existiert nur noch auf dem Client. Den eingebauten Bildschirm hängt macOS dabei als Spiegel an; MacShine setzt seine Helligkeit auf 0 und seine Farbtabelle auf Schwarz (bei Mini-LED komplett dunkel). Nach dem Stream kommt alles zurück. Falls MacShine dabei abstürzt, stellt der nächste Start sie wieder her; notfalls `"/Applications/MacShine.app/Contents/MacOS/sunshine_vd_helper" --restore` oder ab- und wieder anmelden.
- **Feste Identität pro gekoppeltem Client** (aus dem Client-Zertifikat): macOS merkt sich Einstellungen und Anordnung je Gerät.
- Display verschwindet, wenn die App beendet wird (oder Sunshine abstürzt). Resume mit gleicher Auflösung behält das Display.
- **Cmd+Esc am Mac** beendet alle Streams und die laufende App und schaltet die Host-Displays wieder ein (abschaltbar). Löst auch aus, wenn der Client Cmd+Esc schickt.
- Wenn das Display nicht erstellt werden kann, streamt Sunshine wie gewohnt den physischen Bildschirm.

## Bauen
```
git clone -b macos-virtual-display https://github.com/gersiaaan/MacShine.git ~/MacShine
bash ~/MacShine/scripts/build-macshine.sh --deps
```
Ergebnis: `/Applications/MacShine.app`. Braucht Homebrew und Xcode Command Line Tools. Dauert beim ersten Mal lange.

## Einstellungen (Web-UI → Audio/Video, oder sunshine.conf)
| Schlüssel | Standard |
|---|---|
| `macos_virtual_display` | `enabled` |
| `macos_virtual_display_hidpi` | `enabled` |
| `macos_virtual_display_primary` | `enabled` |
| `macos_virtual_display_disable_host` | `disabled` |
| `macos_stop_stream_hotkey` | `enabled` |

## Einschränkungen
- Nutzt die private API `CGVirtualDisplay`; kann nach macOS-Updates brechen.
- Virtuelle Displays sind unter macOS teils auf 60 Hz begrenzt; bei Ablehnung höherer Raten fällt der Helfer auf 60 Hz zurück.
- Ein Stream gleichzeitig (wie Sunshine selbst).
- Host-Displays ausschalten nutzt die private API `CGSConfigureDisplayEnabled`.
- Das Build-Skript signiert mit einem lokalen Zertifikat (`~/Library/Application Support/MacShine/signing`), damit die Freigaben (Bildschirmaufnahme, Bedienungshilfen) Neubauten überstehen. Wird dieser Ordner gelöscht, verlangt macOS sie einmal neu.

## Geänderte Dateien
- neu: `src/platform/macos/vd_helper.m` (Helfer-Prozess, besitzt das Display)
- neu: `src/platform/macos/virtual_display.{h,cpp}` (startet/stoppt den Helfer)
- `src/display_device.cpp` (Session-Start/-Ende, Capture-Ziel)
- `src/config.{h,cpp}`, Web-UI, CMake
