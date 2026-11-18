# MacShine – Sunshine mit virtuellen Displays pro Client für macOS

Sunshine (LizardByte, Commit `7974a40`) plus virtuelle Displays nach Apollo-Prinzip.
Display-Logik angelehnt an OpenDisplay (peetzweg/opendisplay). Beide GPL-3.0, dieser Patch ebenfalls.

## Was es macht
- Beim Stream-Start entsteht ein virtuelles Display in **Auflösung des Clients** (Moonlight-Einstellung), gerendert mit doppelter Client-Bildrate (siehe unten).
- Optional **Retina/HiDPI** (Standard an): Client 2732×2048 → Desktop 1366×1024 pt, gestreamt mit 2732×2048 px, scharfer Text.
- **Darstellungsgröße** (Standard 100 %): wie „Größerer Text … Mehr Platz" in macOS. Der Desktop wird in der gewählten Größe gerendert und auf die Client-Auflösung skaliert; die Stream-Auflösung bleibt gleich. Während eines Streams lässt sich die Größe auch direkt in **Systemeinstellungen → Displays** umstellen (75–200 %); die Wahl gilt sofort und wird pro Gerät gemerkt (`~/Library/Application Support/MacShine/display-sizes`). Die Einstellung in der Weboberfläche gilt für Geräte ohne eigene Wahl.
- Optional **Hauptdisplay** (Standard an): Menüleiste und Dock wandern auf das virtuelle Display → voller Desktop auf dem Client.
- Optional **Host-Displays aus** (Standard aus): Während des Streams werden die eigenen Displays des Macs abgeschaltet, der Desktop existiert nur noch auf dem Client. Den eingebauten Bildschirm hängt macOS dabei als Spiegel an; MacShine setzt seine Helligkeit auf 0 und seine Farbtabelle auf Schwarz (bei Mini-LED komplett dunkel). Displays, die erst während des Streams angehen (Deckel aufgeklappt, Display aufgewacht oder angesteckt), werden sofort ebenfalls abgedunkelt und abgeschaltet. Nach dem Stream kommt alles zurück. Falls MacShine dabei abstürzt, stellt der nächste Start sie wieder her; notfalls `"/Applications/MacShine.app/Contents/MacOS/sunshine_vd_helper" --restore` oder ab- und wieder anmelden.
- **Feste Identität pro gekoppeltem Client** (aus dem Client-Zertifikat): macOS merkt sich Einstellungen und Anordnung je Gerät.
- Display bleibt nach Sessionende (Trennen oder App beenden) noch **30 s** bestehen; verbindet sich der Client in der Zeit neu oder startet die App neu, behält er Display, Größe und Fensteranordnung. Danach wird es entfernt (sofort, wenn Sunshine beendet wird oder abstürzt). Schlägt das Anlegen fehl, versucht MacShine es bis zu dreimal.
- **Tastatur und Trackpad des Macs gesperrt** (Standard an, `macos_block_local_input`): Solange ein Client verbunden ist, ignoriert der Mac seine eigenen Eingaben (auch andere lokale Geräte und Apps). Nur Cmd und Esc kommen durch, damit Cmd+Esc den Stream beenden kann. Braucht die Bedienungshilfen-Berechtigung.
- **Cmd+Esc am Mac** beendet alle Streams und die laufende App, entfernt das virtuelle Display sofort und schaltet die Host-Displays wieder ein (abschaltbar). Löst auch aus, wenn der Client Cmd+Esc schickt.
- Wenn das Display nicht erstellt werden kann, streamt Sunshine wie gewohnt den physischen Bildschirm.

## Mikrofon vom Client (z. B. Artemide Duo)
- Clients, die es können, schicken ihr Mikrofon als Opus über den verschlüsselten Steuerkanal (Pakettyp `0x5510`, Feature-Flag `0x10000`).
- MacShine spielt es in das eigene Eingabegerät **„MacShine"**, ein umbenannter BlackHole-Treiber. Sichtbar ist nur der Eingang; den versteckten Ausgang „MacShine Sender" füttert MacShine.
- Während der Client sendet, ist „MacShine" die Standard-Eingabe des Macs (Claude, Siri, Zoom … hören das Duo). Danach kommt die vorige Eingabe zurück.
- Treiber bauen und installieren (braucht das Admin-Passwort, Ton setzt eine Sekunde aus):
  ```
  scripts/macshine-mic/build-driver.sh
  scripts/macshine-mic/install-driver.sh cmake-build-mic-driver/MacShine.driver
  ```
- Klang: Echounterdrückung (der Stream-Ton wird aus dem Mikrofon herausgerechnet), Apples Sprachisolierung, ein Equalizer Richtung iPhone-Stimme, Pegel +4 dB mit Limiter.
- Einstellungen: `client_mic`, `client_mic_default_input`, `client_mic_echo_cancellation`, `client_mic_voice_isolation`, `client_mic_eq` (alle `enabled`), `client_mic_gain` (`4`, −30 bis 12 dB).

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
| `macos_virtual_display_scale` | `100` (50–200, mehr = mehr Platz, weniger = größere Schrift) |
| `macos_virtual_display_primary` | `enabled` |
| `macos_virtual_display_disable_host` | `disabled` |
| `macos_stop_stream_hotkey` | `enabled` |

## Einschränkungen
- Nutzt die private API `CGVirtualDisplay`; kann nach macOS-Updates brechen.
- Das virtuelle Display läuft mit der doppelten Bildrate des Clients (höchstens 120 Hz, nie unter der Client-Rate), damit Eingaben schneller im nächsten Bild landen; gestreamt wird weiter mit der Client-Rate.
- Virtuelle Displays sind unter macOS teils auf 60 Hz begrenzt; bei Ablehnung höherer Raten fällt der Helfer auf 60 Hz zurück.
- Ein Stream gleichzeitig (wie Sunshine selbst).
- Host-Displays ausschalten nutzt die private API `CGSConfigureDisplayEnabled`.
- Das Build-Skript signiert mit einem lokalen Zertifikat (`~/Library/Application Support/MacShine/signing`), damit die Freigaben (Bildschirmaufnahme, Bedienungshilfen) Neubauten überstehen. Wird dieser Ordner gelöscht, verlangt macOS sie einmal neu.

## Geänderte Dateien
- neu: `src/platform/macos/vd_helper.m` (Helfer-Prozess, besitzt das Display)
- neu: `src/platform/macos/virtual_display.{h,cpp}` (startet/stoppt den Helfer)
- `src/display_device.cpp` (Session-Start/-Ende, Capture-Ziel)
- `src/config.{h,cpp}`, Web-UI, CMake
