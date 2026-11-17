# runs as root on the phone: touch-chip interrupts with pad 72 normal, parked (function 0), restored
irq() { awk '/synaptics_tcm/ { s = 0; for (i = 2; i <= 9; i++) s += $i; print s }' /proc/interrupts; }
win() { a=$(irq); sleep 6; echo $(( $(irq) - a )); }
aoc() { exec 3<>/dev/acd-debug; echo "$1" >&3; sleep 1; timeout 1 cat <&3 | tr -d '\r' | grep -E 'GPIO|rror|nvalid' | head -3; exec 3<&-; }
input keyevent 224
echo "waiting for a finger (irq $(irq))"
s=$(irq); n=0
until [ $(( $(irq) - s )) -gt 20 ]; do sleep 1; n=$((n+1)); [ $((n % 20)) = 0 ] && input keyevent 224; done
echo "A normal: $(win) interrupts in 6 s"
aoc "dbg gpio -n 72 -d 1"
aoc "dbg gpio -n 72 -o 0 -f 0"
aoc "dbg gpio -n 72 -d 1"
echo "B pad 72 on function 0: $(win) interrupts in 6 s"
aoc "dbg gpio -n 72 -o 0 -f 2"
aoc "dbg gpio -n 72 -d 1"
echo "C restored: $(win) interrupts in 6 s"
