#!/bin/sh
# Watches the game on the iPhone: polls until it exits, reports a hang, or the time is up, then prints a summary of its log.
# usage: scripts/phone-watch.sh <seconds> [launch]     ("launch" starts the app first, terminating a running one)
D=${GTA_DEVICE:?set GTA_DEVICE to the phone identifier from: xcrun devicectl list devices}
B=dev.kalki.gta
cd "$(dirname "$0")/.."
pull() { rm -f run/ios-game.log; xcrun devicectl device copy from --device $D --source Documents/game.log --destination run/ios-game.log --domain-type appDataContainer --domain-identifier $B > /dev/null 2>&1; }
[ "$2" = launch ] && xcrun devicectl device process launch --terminate-existing --device $D $B 2>&1 | tail -1 | cut -c1-160
end=$(( $(date +%s) + $1 ))
why="time up"
while [ "$(date +%s)" -lt "$end" ]; do
  sleep 20
  if ! xcrun devicectl device info processes --device $D 2>/dev/null | grep -q 'GTA.app'; then why="process gone"; break; fi
  pull
  if LC_ALL=C grep -q 'hang report' run/ios-game.log 2>/dev/null; then why="hang report"; break; fi
done
pull
echo "== $why; last host time: $(LC_ALL=C grep -E '^\[host' run/ios-game.log | tail -1 | cut -c1-16)"
LC_ALL=C grep -E '\[gpu\] fps|null renderer:' run/ios-game.log | LC_ALL=C sed -E 's/.*\[host +([0-9.]+)\] (\[gpu\] fps: ([0-9.]+) \(([0-9]+) frames, ([0-9]+) draws.*|null renderer: ([0-9]+) frames \(([0-9.]+) fps.*)/t=\1 fps=\3\7 draws=\5/' | tail -3
LC_ALL=C grep -E 'memory still' run/ios-game.log | tail -1 | cut -c1-80
LC_ALL=C grep -n 'engine trap\|Fatal Error\|OutOfMemory$\|hang report\|autoplay' run/ios-game.log | head -6 | cut -c1-160
LC_ALL=C grep 'frame profile, main thread' run/ios-game.log | tail -1 | cut -c1-230
