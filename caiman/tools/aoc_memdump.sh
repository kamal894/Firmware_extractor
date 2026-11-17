# usage: aocmem2.sh cmdfile ; sends each line of cmdfile to the AOC console and
# prints the dump lines. Addresses are written on the Mac: Android's sh does
# 32-bit signed arithmetic, and a sign-extended address crashes the AOC console.
ls -l /proc/[0-9]*/fd 2>/dev/null | grep -q acd-debug && { echo busy; exit 1; }
exec 3<>/dev/acd-debug || exit 1
timeout 1 cat <&3 >/dev/null
while read -r c; do
  case "$c" in "mem dump 0x"[0-9a-f]*" "[0-9]*) ;; *) echo "bad line: $c"; exit 1;; esac
  echo "$c" >&3 || exit 1
  sleep 0.6; timeout 0.8 cat <&3 | tr -d '\r' | grep -E '^  0x|rror|ault|Bad'
done < "$1"
exec 3<&-
