#!/bin/sh
# Guest-side rome benchmark/smoke harness (QEMU or Pi).
# env: OUT (results dir), R (Rome binary), T (Terminal binary), N (scroll lines),
#      XPID (X server pid), DISPLAY, SIZES ("80x24 150x45"), RENDERERS ("x11 gl")
OUT=${OUT:-/var/romebench}
rm -rf $OUT; mkdir -p $OUT
export DISPLAY=${DISPLAY:-:0}
R=${R:-/Applications/Rome.app/Rome}
T=${T:-/Applications/Terminal.app/Terminal}
N=${N:-5000}
SIZES=${SIZES:-"80x24 150x45"}
RENDERERS=${RENDERERS:-"x11 gl"}
ps ax | grep -v grep | grep -q gdnc || { /usr/local/bin/gdnc --auto --daemon > $OUT/gdnc.log 2>&1 & sleep 20; }
echo "RB start"
XPID=${XPID:-$(ps ax | awk '/Xvfb|Xorg/ && !/awk/ {print $1; exit}')}
export ROME_STATS=1
# cpu <pid>: the TIME column of ps ax, as seconds
cpu() { ps ax | awk -v p=$1 '$1==p {n=split($4,a,":"); s=0; for(i=1;i<=n;i++) s=s*60+a[i]; print s; exit}'; }

# wait_for <file> <grep pattern> <max seconds>
wait_for() {
	n=0
	while [ $n -lt $3 ]; do grep -q "$2" $1 2>/dev/null && return 0; sleep 1; n=$((n+1)); done
	return 1
}

# run_scroll <label> <app> <args...> -- <how to build the command>: see below
# scroll <label> <seconds budget> <app-with-args-prefix...>
# The workload command prints its own elapsed seconds into $OUT/<label>.t and
# touches $OUT/<label>.done; the X server's and the app's CPU seconds are
# read around it.
scroll() {
	label=$1; shift
	cmd="TIMEFORMAT=%R; { time seq 1 $N; } 2> $OUT/$label.t; touch $OUT/$label.done; sleep 1"
	x0=$(cpu $XPID)
	"$@" "$cmd" > $OUT/$label.log 2>&1 &
	pid=$!
	n=0
	while [ ! -e $OUT/$label.done ] && [ $n -lt 900 ]; do sleep 1; n=$((n+1)); done
	a1=$(cpu $pid); x1=$(cpu $XPID)
	echo "RB scroll_$label seconds=$(cat $OUT/$label.t) app_cpu_s=$a1 xserver_cpu_s=$((x1-x0)) (N=$N)"
	sleep 3; grep "rome-stats" $OUT/$label.log | tail -1 | sed "s/^/RB scroll_$label /"
	kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
}

# top <label> <app...>: ten seconds of top -s 1 on the screen
topwl() {
	label=$1; shift
	cmd="top -s 1 & p=\$!; sleep 14; kill \$p; touch $OUT/$label.done; sleep 1"
	x0=$(cpu $XPID)
	"$@" "$cmd" > $OUT/$label.log 2>&1 &
	pid=$!
	n=0
	while [ ! -e $OUT/$label.done ] && [ $n -lt 300 ]; do sleep 1; n=$((n+1)); done
	a1=$(cpu $pid); x1=$(cpu $XPID)
	sleep 3
	echo "RB top_$label app_cpu_s=$a1 xserver_cpu_s=$((x1-x0)) (14 s of top -s 1)"
	grep "rome-stats" $OUT/$label.log | tail -1 | sed "s/^/RB top_$label /"
	kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
}

shot() {
	label=$1; shift
	"$@" > $OUT/shot_$label.log 2>&1 &
	pid=$!
	wait_for $OUT/shot_$label.log "first frame" 300 || wait_for $OUT/shot_$label.log "rome-stats" 5
	sleep 15
	/usr/X11/bin/xwd -root -silent -out $OUT/shot_$label.xwd
	echo "RB shot_$label $(ls -l $OUT/shot_$label.xwd | awk '{print $5}') bytes"
	kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
}

SHOT='printf "\033[1mbold\033[0m \033[31mred\033[0m \033[42;30mgreen bg\033[0m \033[4munder\033[0m \033[7mreverse\033[0m \342\224\214\342\224\200\342\224\200\342\224\220 \342\226\200\342\226\204\342\226\210\342\226\221\342\226\222\342\226\223 caf\303\251 \316\273\n\033[38;5;208m256-colour\033[0m \033[38;2;10;120;250mtruecolour\033[0m\n"; ls -la /Applications; seq 1 3; printf "\\033[1mbold\\033[0m \\033[31mred\\033[0m \\033[42;30mgreen bg\\033[0m \\033[4munder\\033[0m \\033[7mreverse\\033[0m \\342\\224\\214\\342\\224\\200\\342\\224\\200\\342\\224\\220 \\342\\226\\200\\342\\226\\204\\342\\226\\210\\342\\226\\221\\342\\226\\222\\342\\226\\223 caf\\303\\251 \\316\\273\\n\\033[38;5;208m256-colour\\033[0m \\033[38;2;10;120;250mtruecolour\\033[0m\\n"; sleep 600'
if [ -z "$NOSHOT" ]; then
	for r in $RENDERERS; do
		shot $r $R -RomeRenderer $r -RomeCommand "$SHOT"
	done
fi
[ -n "$SHOTONLY" ] && { echo RB-DONE; exit 0; }

for r in $RENDERERS; do
	for size in $SIZES; do
		c=${size%x*}; rw=${size#*x}; L=${r}_$size
		scroll $L $R -RomeRenderer $r -RomeColumns $c -RomeRows $rw -RomeQuitOnExit YES -RomeCommand
		topwl $L $R -RomeRenderer $r -RomeColumns $c -RomeRows $rw -RomeQuitOnExit YES -RomeCommand
		$R -RomeRenderer $r -RomeColumns $c -RomeRows $rw -RomeCommand cat -RomeBenchType 60 -RomeQuitOnExit YES > $OUT/type_$L.log 2>&1 &
		p=$!; n=0
		while kill -0 $p 2>/dev/null && [ $n -lt 300 ]; do sleep 1; n=$((n+1)); done
		kill -9 $p 2>/dev/null
		grep rome-stats $OUT/type_$L.log | tail -1 | sed "s/^/RB type_$L /"
	done
done
# the existing Terminal.app (its window is its own default size, 80x25)
scroll terminal $T
topwl terminal $T
echo RB-DONE
