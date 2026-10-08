#!/bin/sh
# Rome against the terminal it replaces (GNUstep's Terminal.app), same machine, same
# priority, measured from outside through the X server. Run it from a shell in a
# desktop terminal (not over ssh: sshd's children are background class here, which
# makes every timer up to 100 ms late for both programs).
#
#   sh compare.sh [OUTDIR]       results in OUTDIR/results.txt, then OUTDIR/all.done
#
# Needs xlatency (tools/build_xlatency.sh) in /tmp, DISPLAY and XAUTHORITY.
OUT=${1:-/tmp/rome-compare}
N=${N:-50000}
mkdir -p "$OUT"; rm -f "$OUT"/*.done
export DISPLAY=${DISPLAY:-:0} XAUTHORITY=${XAUTHORITY:-/var/lib/gsdm/auth-0}
X=/tmp/xlatency
RESULT="$OUT/results.txt"
: > "$RESULT"
say() { echo "$*" >> "$RESULT"; }

# cpu_s <pid>: cumulative CPU seconds from ps TIME (M:SS.cc or H:MM:SS)
cpu_s() { ps -o time= -p "$1" 2>/dev/null | awk '{n=split($1,a,":"); s=0; for(i=1;i<=n;i++) s=s*60+a[i]; print s}'; }
xpid=$(ps -ax -o pid=,command= | awk '/Xorg :0/ && !/awk/ {print $1; exit}')

say "== environment"
say "shell priority: $(ps -o pri= -p $$)   (31 = desktop, 4 = background class)"
say "N (scroll lines): $N"
say "busy processes at start:"
ps -ax -o pcpu=,command= | awk '$1 > 3 {print "   " $0}' | head -5 >> "$RESULT"

launch() {   # launch <name> -> sets APID
	name=$1
	case $name in
	rome) /Applications/Rome.app/Rome -RomeCursorBlink NO > "$OUT/$name.log" 2>&1 & ;;
	terminal) /Applications/Terminal.app/Terminal > "$OUT/$name.log" 2>&1 & ;;
	esac
	APID=$!
	startup=$($X waitwin $APID 30000)
	sleep 4         # the shell's first prompt
	say "$name: pid $APID, priority $(ps -o pri= -p $APID), $startup"
}

for name in rome terminal; do
	say ""; say "== $name"
	launch $name
	$X type $APID "export PS1='$ '; TIMEFORMAT=%R; clear"
	sleep 2

	# 1. scroll throughput: N lines through the terminal, wall seconds from bash's `time`
	for run in 1 2 3; do
		rm -f "$OUT/$name.scroll.done"
		c0=$(cpu_s $APID); x0=$(cpu_s $xpid)
		$X type $APID "( time seq 1 $N ) 2> $OUT/$name.scroll.$run.t; touch $OUT/$name.scroll.done"
		i=0; while [ ! -e "$OUT/$name.scroll.done" ] && [ $i -lt 600 ]; do sleep 1; i=$((i+1)); done
		c1=$(cpu_s $APID); x1=$(cpu_s $xpid)
		say "scroll $N lines, run $run: $(cat $OUT/$name.scroll.$run.t 2>/dev/null || echo timeout) s, terminal cpu $(echo "$c1 $c0" | awk '{printf "%.1f", $1-$2}') s, X server cpu $(echo "$x1 $x0" | awk '{printf "%.1f", $1-$2}') s"
		$X type $APID "clear"; sleep 1
	done

	# 2. idle CPU: 30 s with a prompt on screen
	sleep 3
	c0=$(cpu_s $APID); x0=$(cpu_s $xpid)
	sleep 30
	c1=$(cpu_s $APID); x1=$(cpu_s $xpid)
	say "idle 30 s: terminal cpu $(echo "$c1 $c0" | awk '{printf "%.2f", $1-$2}') s, X server cpu $(echo "$x1 $x0" | awk '{printf "%.2f", $1-$2}') s"
	say "memory (RSS): $(ps -o rss= -p $APID) KB"

	# 3. key to pixels, with `cat` echoing into the top row
	say "$($X floor $APID)"
	for run in 1 2 3; do
		$X type $APID "clear; cat"
		sleep 2
		say "latency run $run: $($X latency $APID 60 200 2>&1 | tr '\n' ' ')"
		$X type $APID ""      # Return, then Control-D: cat ends and the shell is back
		$X ctrld $APID
		sleep 1
	done
	kill $APID 2>/dev/null; sleep 2; kill -9 $APID 2>/dev/null
done
say ""; say "done"
touch "$OUT/all.done"
