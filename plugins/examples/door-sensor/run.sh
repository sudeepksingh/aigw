#!/bin/sh
# Example AIGW plug-in in plain shell (works with busybox ash on OpenWRT too).
#
# AIGW starts this program and talks to it in JSON lines:
#   stdin   {"hello": {...}}, {"event": {...}}, {"act": {...}}, {"undo": {...}}
#   stdout  {"emit": "iot.door", "body": {...}}, {"done": {...}}, {"alert": {...}}
# stderr goes to the AIGW log. Install it with:  sudo agctl plugin add plugins/examples/door-sensor
#
# A real sensor would read a GPIO, a serial port or a radio here; this one
# pretends the front door opens and closes.

emitter=""

start_emitter() {
	interval=$1
	(
		open=false
		while :; do
			printf '{"emit":"iot.door","body":{"door":"front","open":%s}}\n' "$open"
			if [ "$open" = false ]; then open=true; else open=false; fi
			sleep "$interval"
		done
	) &
	emitter=$!
}

field() { # field <json line> <name>: first number or string value of "name"
	printf '%s\n' "$1" | sed -n "s/.*\"$2\": *\"\{0,1\}\([^\",}]*\).*/\1/p" | head -n 1
}

while IFS= read -r line; do
	case $line in
	*'"hello"'*)
		interval=$(field "$line" interval)
		echo "door-sensor: started, reporting every ${interval:-30}s" >&2
		[ -n "$emitter" ] || start_emitter "${interval:-30}"
		;;
	*'"act"'*)
		id=$(field "$line" id)
		ip=$(field "$line" ip)
		echo "door-sensor: flashing the hall light for $ip (decision $id)" >&2
		printf '{"done":{"id":%s,"ok":true}}\n' "$id"
		;;
	*'"undo"'*)
		echo "door-sensor: light back to normal (decision $(field "$line" id))" >&2
		;;
	esac
done

[ -n "$emitter" ] && kill "$emitter" 2>/dev/null
exit 0
