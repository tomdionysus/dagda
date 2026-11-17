#!/bin/bash
#
# Dagda - The Distributed Object Store
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Harness workload 7 (repair), across two Pis: the controller, run from the development
# machine over ssh. It only drives: nothing is built or measured here. Node a runs on $A
# and holds; node b runs on $B and joins. Both stores' harness binaries and their
# libraries are under /root/harness on both Pis at the same paths (built on one, copied to
# the other): macha-build/harness-macha and build-tip/harness-dagda. An older Dagda, store
# name dagda-<name>, runs from build-<name>/ (its harness-dagda and dagda/libdagda.so.*,
# kept from an earlier build-tip): dagda-old is the baseline's, dagda-s3 stage 3's. So a
# repair stage is measured old against new in one session; its figures carry that name.
#
#   pi-repair-run.sh smoke        one tiny rejoin a store: does the driver work across the Pis
#   pi-repair-run.sh link         the raw link's rate and round trip now
#   pi-repair-run.sh run          the three rounds
#   pi-repair-run.sh uncount      remove the counting filters from both Pis (also done on exit)
#
# Settings, from the environment: A and B, the two Pis' addresses; DEV, their interface;
# OUT, where the figures go (build/harness/results/<date> in this repository by default);
# STORES, the stores smoke and run compare (macha dagda by default); REST, the seconds
# between runs; SCENARIOS, which of rejoin-large, rejoin-small, away-large, away-small and
# viewer a run takes (all by default); NOCOLD=1, which skips the fstrim and the page-cache drop before each run
# and must be set on any Pi not held whole (the drop costs every other job there its
# cache); TRACE=1, which has a Dagda node write repair's decisions and a sample of its
# credit every quarter second to its stderr (repair-err.txt here), for finding a
# mechanism and never in a measured run.
#
# Before a run: claim on the board the cores it uses on both Pis (a node needs about one;
# the runs are bound by the link), with an honest end time, and post a note: the traffic
# slows every session's ssh and rsync to those Pis.
#
# Figures: $OUT/repair.jsonl, each line's workload relabelled "repair <scenario> <size>
# round<N> <node>"; the wire figures come from a counting filter on each Pi, the same
# measurement for both stores.
set -u
A=${A:-192.0.2.2}; B=${B:-192.0.2.3}; DEV=${DEV:-wlan0}
R=/root/harness; W=$R/repair
PA=7611; PB=7612
HERE=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$HERE/build/harness/results/$(date -u +%Y-%m-%d)}
mkdir -p "$OUT"; RES=$OUT/repair.jsonl; LOG=$OUT/repair-status.txt
say() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$LOG"; }
on() { h=$1; shift; ssh -o BatchMode=yes root@$h "$@"; }
dir() { if [ "$1" = a ]; then echo $W/a; else echo $W/b; fi; }
host() { if [ "$1" = a ]; then echo $A; else echo $B; fi; }

# The maintainer, 2026-11-16: two Pis and the WiFi between them as it is, unshaped (0.78 MB/s when
# measured, 2.4 GHz through the access point, power save on). What each Pi sends the other
# is counted by a filter on the interface's egress hook whose action is to pass: it counts
# and changes nothing, and the root qdisc is left alone. Removed on exit.
count_on() {
  for n in a b; do
    h=$(host $n); if [ $n = a ]; then peer=$B; else peer=$A; fi
    on $h "tc qdisc del dev $DEV clsact 2>/dev/null; tc qdisc add dev $DEV clsact && tc filter add dev $DEV egress protocol ip u32 match ip dst $peer/32 action pass" || { say "counting filter failed on $h"; return 1; }
  done
}
count_off() { for h in $A $B; do on $h "tc qdisc del dev $DEV clsact 2>/dev/null; true"; done; }
# Bytes and packets a Pi has sent the other so far, as "bytes packets"; "0 0" with no filter.
wire() { on $(host $1) "tc -s filter show dev $DEV egress 2>/dev/null | awk '/Sent/{print \$2, \$4; f=1; exit} END{if(!f) print \"0 0\"}'"; }
# The raw link now: ten seconds of one TCP stream a to b, then the round trip.
link() {
  on $B "nohup python3 -c 'import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); s.bind((\"$B\",7699)); s.listen(1)
c,_=s.accept(); n=0; t=time.time()
while True:
    d=c.recv(1<<20)
    if not d: break
    n+=len(d)
print(round(n/(time.time()-t)/1e6,3))' > $R/link.txt 2>&1 < /dev/null & sleep 1"
  on $A "python3 -c 'import socket,time
s=socket.create_connection((\"$B\",7699)); b=bytes(1<<16); t=time.time()
while time.time()-t<10: s.sendall(b)
s.close()'"
  sleep 2
  mbs=$(on $B "cat $R/link.txt")
  rtt=$(on $A "ping -c 20 -i 0.2 -q $B | tail -1 | cut -d= -f2")
  say "raw link $1: $mbs MB/s, round trip min/avg/max/mdev$rtt"
  echo "{\"store\":\"link\",\"workload\":\"repair link $1\",\"figure\":\"link.throughput\",\"value\":$mbs,\"unit\":\"MB/s\"}" >> "$RES"
}

# The node's answers so far are cleared first, so a wait sees only this command's.
cmd() { n=$1; shift; printf '%s\n' "$@" | on $(host $n) "cat > $(dir $n)/cmd.tmp && : > $(dir $n)/log && mv $(dir $n)/cmd.tmp $(dir $n)/cmd"; }
# Waits for an answer line in the node's log: "done <what>" or "failed <what>".
waitfor() {
  n=$1; what=$2; limit=${3:-3600}
  on $(host $n) "t=0; until grep -qE '^(done|failed) $what' $(dir $n)/log 2>/dev/null; do sleep 1; t=\$((t+1)); if [ \$t -gt $limit ]; then echo timeout; exit 1; fi; pgrep -f '[h]arness-.* repair-node' > /dev/null || { echo 'the node is gone'; exit 1; }; done; grep -E '^failed $what' $(dir $n)/log && exit 1; exit 0" || { say "waiting for $n '$what' failed"; return 1; }
}
start() {
  store=$1; n=$2
  d=$(dir $n); h=$(host $n)
  if [ $n = a ]; then p=$PA; seed=""; else p=$PB; seed="$A $PA"; fi
  case $store in
    macha) bin="$R/macha-build/harness-macha $d repair-node $n $h $p $seed" ;;
    dagda-*) old=$R/build-${store#dagda-}; bin="env LD_LIBRARY_PATH=$old/dagda $old/harness-dagda repair-node $d $n $h $p $seed" ;;
    *) bin="$R/build-tip/harness-dagda repair-node $d $n $h $p $seed" ;;
  esac
  on $h "cd $d && (setsid nohup ${TRACE:+env HARNESS_REPAIR_TRACE=1} $bin >> $d/out 2>> $d/err < /dev/null &)"
}
stop() { cmd $1 stop; on $(host $1) "t=0; while pgrep -f '[h]arness-.* repair-node' > /dev/null; do sleep 1; t=\$((t+1)); [ \$t -gt 120 ] && { pkill -9 -f '[h]arness-.* repair-node'; break; }; done; true"; }
copy() { on $A "cat $W/a/$1" | on $B "mkdir -p \$(dirname $W/b/$1) && cat > $W/b/$1"; }
# What b needs of a's to be the same cluster, and the objects the run is about.
share() {
  store=$1
  if [ $store != macha ]; then on $A "until [ -f $W/a/cluster/ca.pem ]; do sleep 0.2; done"; copy cluster/ca.key; copy cluster/ca.pem; fi
  copy ids
}
fresh() {
  store=$1
  for h in $A $B; do on $h "pkill -9 -f '[h]arness-.* repair-node' 2>/dev/null; rm -rf $W; mkdir -p $W/a $W/b; [ -n '${NOCOLD:-}' ] || { fstrim / > /dev/null 2>&1; sync; echo 3 > /proc/sys/vm/drop_caches; }"; done
  if [ $store = macha ]; then on $A "head -c 48 /dev/urandom | base64 > $W/a/cluster.key"; copy cluster.key; fi
  sleep ${REST:-30}
}
# Each line relabelled with the run's workload and, for dagda-old, its store name (the
# binary's own begins "dagda", then its versions).
collect() {
  label=$1; store=$2
  for n in a b; do
    on $(host $n) "cat $(dir $n)/out; cat $(dir $n)/err >&2" 2>> "$OUT/repair-err.txt" | sed -e "s/\"workload\":\"[^\"]*\"/\"workload\":\"$label $n\"/" -e "s/\"store\":\"dagda/\"store\":\"$store/" >> "$RES"
  done
}
wirefig() {
  label=$1; store=$2; n=$3; before=$4; after=$5
  set -- $before $after
  echo "{\"store\":\"$store\",\"workload\":\"$label $n\",\"figure\":\"wire.bytes_sent\",\"value\":$(( $3 - $1 )),\"unit\":\"B\"}" >> "$RES"
  echo "{\"store\":\"$store\",\"workload\":\"$label $n\",\"figure\":\"wire.packets_sent\",\"value\":$(( $4 - $2 )),\"unit\":\"packets\"}" >> "$RES"
}

# b rejoins empty: a alone is filled; b starts with nothing and the same objects named.
# With `viewer`, a foreground reader on a reads one object a second, alone for 30 s first.
rejoin() {
  store=$1; size_label=$2; count=$3; size=$4; round=$5; viewer=${6:-}
  # Under a viewer repair is on its weighted share: bounded at twenty minutes, and what
  # had arrived by then is the figure.
  bound=7200; [ -n "$viewer" ] && bound=1200
  label="repair rejoin${viewer:+-viewer} $size_label round$round"
  fresh $store; start $store a
  cmd a "fill 1000 $count $size" && waitfor a "fill" && cmd a reference && waitfor a reference || return 1
  share $store
  if [ -n "$viewer" ]; then cmd a viewer-start; sleep 30; cmd a "viewer-stop viewer_alone"; waitfor a "viewer-stop"; cmd a viewer-start; fi
  wa=$(wire a); wb=$(wire b)
  cmd b peer reference "measure rejoin $bound"; start $store b
  waitfor b "measure rejoin" $((bound + 600)) || { collect "$label FAILED" $store; stop a; stop b; return 1; }
  if [ -n "$viewer" ]; then cmd a "viewer-stop viewer_loaded"; waitfor a "viewer-stop viewer_loaded"; fi
  cmd a "figures holder"; waitfor a "figures holder"
  wirefig "$label" $store a "$wa" "$(wire a)"; wirefig "$label" $store b "$wb" "$(wire b)"
  stop b; stop a; collect "$label" $store
  say "$label $store: $(grep -E "\"store\":\"$store[ \"].*\"workload\":\"$label b\".*rejoin.seconds" "$RES" | tail -1 | sed 's/.*"value":\([^,]*\).*/\1/') s"
}

# b away, then back: both hold the first half; b stops, keeping its disk; a is given the
# second half; b starts again.
away() {
  store=$1; size_label=$2; count=$3; size=$4; round=$5
  half=$((count / 2)); label="repair away-back $size_label round$round"
  fresh $store; start $store a
  cmd a "fill 1000 1 4096" && waitfor a fill || return 1
  share $store
  cmd b peer; start $store b; waitfor b peer 600 || return 1
  on $A "rm -f $W/a/ids"
  cmd a "fill 2000 $half $size" && waitfor a "fill 2000" && cmd a reference && waitfor a reference || return 1
  copy ids
  cmd b reference "measure first"; waitfor b "measure first" 7200 || { collect "$label FAILED" $store; stop a; stop b; return 1; }
  stop b
  # Until a counts b dead (dead_after 30 s), so its writes are not waiting on it.
  sleep 40
  cmd a "fill 500000 $half $size" && waitfor a "fill 500000" && cmd a reference && waitfor a "reference" || return 1
  copy ids
  wa=$(wire a); wb=$(wire b)
  cmd b peer reference "measure back"; start $store b
  waitfor b "measure back" 7200 || { collect "$label FAILED" $store; stop a; stop b; return 1; }
  cmd a "figures holder"; waitfor a "figures holder"
  wirefig "$label" $store a "$wa" "$(wire a)"; wirefig "$label" $store b "$wb" "$(wire b)"
  stop b; stop a; collect "$label" $store
  say "$label $store: $(grep -E "\"store\":\"$store[ \"].*\"workload\":\"$label b\".*back.seconds" "$RES" | tail -1 | sed 's/.*"value":\([^,]*\).*/\1/') s"
}

machine() { for h in $A $B; do say "$(on $h 'echo $(hostname) $(uname -r) page $(getconf PAGESIZE) up $(cut -d" " -f1 /proc/uptime)s throttled $(vcgencmd get_throttled | cut -d= -f2)')"; done; }

case ${1:-} in
  uncount) count_off ;;
  link) machine; link now ;;
  smoke)
    machine; trap count_off EXIT; count_on || exit 1
    REST=2
    for store in ${STORES:-macha dagda}; do rejoin $store smoke 20 65536 0 || say "smoke $store FAILED"; done
    grep -E 'smoke' "$RES" | grep -E 'rejoin\.(seconds|complete|objects_needed|copies|repair_bytes|throughput)|reference.seconds|holder\.copies|wire' | sed 's/.*"store":"\([a-z]*\).*"workload":"\([^"]*\)".*"figure":"\([^"]*\)","value":\([^,]*\).*/\1 | \2 | \3 \4/' | tail -40 ;;
  run)
    # A tenth of the plan's sizes, for a link of under 1 MB/s: 26 x 4 MiB (104 MiB) and
    # 410 x 64 KiB (26 MiB).
    trap count_off EXIT
    machine; count_on || exit 1
    for round in 1 2 3; do
      link "round$round"
      set -- ${STORES:-macha dagda}
      if [ $((round % 2)) -eq 1 ]; then order="$1 ${2:-}"; else order="${2:-} $1"; fi
      for scenario in ${SCENARIOS:-rejoin-large rejoin-small away-large away-small viewer}; do
        for store in $order; do
          case $scenario in
            rejoin-large) rejoin $store large 26 4194304 $round ;;
            rejoin-small) rejoin $store small 410 65536 $round ;;
            away-large) away $store large 26 4194304 $round ;;
            away-small) away $store small 410 65536 $round ;;
            viewer) rejoin $store large 26 4194304 $round viewer ;;
          esac
        done
      done
    done
    link after; machine; say donerepair ;;
  *) echo "smoke | link | run | uncount"; exit 2 ;;
esac
