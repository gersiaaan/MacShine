#!/bin/bash
# Installs MacShine.driver into the system HAL plug-in folder and restarts Core Audio.
# Needs an administrator password (macOS asks for it). All audio drops for a second.
set -euo pipefail

DRIVER="${1:?usage: install-driver.sh path/to/MacShine.driver}"
osascript -e "do shell script \"rm -rf /Library/Audio/Plug-Ins/HAL/MacShine.driver && cp -R '${DRIVER}' /Library/Audio/Plug-Ins/HAL/ && chown -R root:wheel /Library/Audio/Plug-Ins/HAL/MacShine.driver && killall -9 coreaudiod\" with administrator privileges"
