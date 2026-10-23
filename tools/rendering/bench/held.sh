#!/bin/bash
# Summarize the last N frame-interval windows (held room view) of a run's game.log.
log=$1; n=${2:-12}
grep 'Native frame intervals' "$log" | tail -$n | sed -E 's/.*median=([0-9.]+).*p95=([0-9.]+).*final_operations=([0-9]+), query_wait_ms_per_frame=([0-9.]+), query_waits_per_frame=([0-9.]+)(, direct_result_latency_us=([0-9.]+), direct_backlog_at_wait=([0-9.]+))?.*/\1 \2 \3 \4 \5 \7 \8/' | awk '{m+=$1;p+=$2;o+=$3;w+=$4;q+=$5;l+=$6;b+=$7;n++} END{printf "median=%.2f ms p95=%.2f ops=%.0f qwait=%.2f ms waits/frame=%.1f latency=%.0f us backlog=%.1f (windows=%d)\n",m/n,p/n,o/n,w/n,q/n,l/n,b/n,n}'
