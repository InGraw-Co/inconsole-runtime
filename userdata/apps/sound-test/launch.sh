#!/bin/sh
set -eu

PATH=/usr/sbin:/usr/bin:/sbin:/bin
LOG_DIR=/userdata/system/logs
LOG_FILE="$LOG_DIR/sound-test.log"

mkdir -p "$LOG_DIR"
touch "$LOG_FILE"

log() {
    printf '%s [sound-test] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG_FILE"
}

if [ ! -x /usr/bin/amixer ]; then
    log "Missing /usr/bin/amixer"
    exit 1
fi

if [ ! -x /usr/bin/speaker-test ]; then
    log "Missing /usr/bin/speaker-test"
    exit 1
fi

log "Starting sound test"
log "PATH=$PATH"
log "uid=$(id -u) gid=$(id -g) pwd=$(pwd)"

if [ -d /dev/snd ]; then
    log "Device nodes under /dev/snd:"
    ls -l /dev/snd | tee -a "$LOG_FILE"
else
    log "/dev/snd directory is missing"
fi

log "ALSA cards before unmute:"
/usr/bin/aplay -l 2>&1 | tee -a "$LOG_FILE" || true

log "Mixer controls before unmute:"
/usr/bin/amixer scontrols 2>&1 | tee -a "$LOG_FILE" || true

log "Key mixer states before unmute:"
for ctl in \
    "Headphone Playback Volume" \
    "DAC Front Playback Volume" \
    "DAC Playback Volume" \
    "Headphone Playback Switch" \
    "Speaker Playback Switch"; do
    /usr/bin/amixer sget "$ctl" 2>&1 | tee -a "$LOG_FILE" || true
done

log "Unmuting common mixer controls"
for ctl in \
    "Headphone Playback Volume" \
    "DAC Front Playback Volume" \
    "DAC Playback Volume" \
    Master Speaker Headphone PCM DAC; do
    /usr/bin/amixer -q sset "$ctl" 100% >/dev/null 2>&1 || true
done
for ctl in "Headphone Playback Switch" "Speaker Playback Switch"; do
    /usr/bin/amixer -q sset "$ctl" on >/dev/null 2>&1 || true
done

log "Mixer controls after unmute:"
/usr/bin/amixer scontrols 2>&1 | tee -a "$LOG_FILE" || true

log "Key mixer states after unmute:"
for ctl in \
    "Headphone Playback Volume" \
    "DAC Front Playback Volume" \
    "DAC Playback Volume" \
    "Headphone Playback Switch" \
    "Speaker Playback Switch"; do
    /usr/bin/amixer sget "$ctl" 2>&1 | tee -a "$LOG_FILE" || true
done

log "Starting ALSA speaker test on default device"
if ! /usr/bin/speaker-test -D default -c 2 -t wav -l 1; then
    log "Default device failed, trying hw:0,0"
    /usr/bin/speaker-test -D hw:0,0 -c 2 -t wav -l 1
fi

log "Sound test finished"
exit 0
