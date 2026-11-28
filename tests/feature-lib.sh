# The features a build has (docs/features.md), for the shell tests.
#
#   . "$(dirname "$0")/feature-lib.sh"
#   needs "$BIN" mesh proactive     # not all built in: say SKIP and exit 0
#   if has mesh; then ...; fi       # a part of a test for one feature
#
# Feature names as agd --features prints them: wifi-heal, mesh, coverage, ...
FEATURES_BUILT=""

features_of() {   # features_of <build dir>
	FEATURES_BUILT=",$("$1/agd/agd" --features 2>/dev/null),"
}

has() {
	for f; do
		case "$FEATURES_BUILT" in *",$f,"*) ;; *) return 1 ;; esac
	done
	return 0
}

# After agd (re)starts: wait until its ubus objects are published and answer,
# before a test asks it anything. 0 when all answer within <seconds>; else 1,
# with what was missing on stderr.
#   ubus_ready <ubus socket> <seconds> ag ag.wifi ag.decision ag.dm
ubus_ready() {
	_sock=$1 _secs=$2
	shift 2
	_ubus=${UBUS:-$(command -v ubus || echo /usr/local/bin/ubus)}
	_i=0
	while :; do
		_missing=""
		for _o; do
			# published, and its owner answers a call: "method not found" (3) for
			# this made-up method means agd's loop is running and served it
			"$_ubus" -s "$_sock" -t 2 list "$_o" 2>/dev/null | grep -q . || { _missing="$_missing $_o"; continue; }
			_rc=0
			"$_ubus" -s "$_sock" -t 2 call "$_o" aigw_ready_probe > /dev/null 2>&1 || _rc=$?
			case $_rc in 0 | 2 | 3) ;; *) _missing="$_missing $_o(call: $_rc)" ;; esac
		done
		[ -z "$_missing" ] && return 0
		_i=$((_i + 1))
		if [ $_i -gt $((_secs * 4)) ]; then
			echo "ubus_ready: after ${_secs} s, not answering on $_sock:$_missing" >&2
			return 1
		fi
		sleep 0.25
	done
}

needs() {   # needs <build dir> <feature>...
	features_of "$1"
	shift
	for f; do
		has "$f" && continue
		echo "$(basename "$0" .sh | tr a-z- A-Z\ ) SKIP: needs $f, not built in (AIGW_FEATURE_$(echo "$f" | tr a-z- A-Z_))"
		exit 0
	done
}
