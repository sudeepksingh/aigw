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

needs() {   # needs <build dir> <feature>...
	features_of "$1"
	shift
	for f; do
		has "$f" && continue
		echo "$(basename "$0" .sh | tr a-z- A-Z\ ) SKIP: needs $f, not built in (AIGW_FEATURE_$(echo "$f" | tr a-z- A-Z_))"
		exit 0
	done
}
