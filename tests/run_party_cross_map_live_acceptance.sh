#!/usr/bin/env bash

set -euo pipefail

usage()
{
	cat <<'EOF'
usage: run_party_cross_map_live_acceptance.sh <port>

Runs the POSIX-only two-client party/Social/chat acceptance test in an isolated
output directory. It verifies UI-model create/invite/accept/promote/kick flows,
authenticated Party chat across divergent maps, unchanged Global chat,
ordinary ENTU map isolation, graceful persistence, and restoration of the
same PartyID.
EOF
}

if [[ $# -ne 1 ]]
then
	usage >&2
	exit 2
fi

port=$1
if [[ ! $port =~ ^[0-9]+$ || $port -lt 1 || $port -gt 65535 ]]
then
	echo "port must be between 1 and 65535" >&2
	exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)
build_dir=${BARONY_LIVE_BUILD_DIR:-"$repo_root/build"}
if [[ ! -d $build_dir ]]
then
	echo "live build directory does not exist: $build_dir" >&2
	exit 2
fi
build_dir=$(cd -- "$build_dir" && pwd)
data_dir=${BARONY_LIVE_DATA_DIR:-$build_dir}
if [[ ! -d $data_dir ]]
then
	echo "live data directory does not exist: $data_dir" >&2
	exit 2
fi
data_dir=$(cd -- "$data_dir" && pwd)
barony="$build_dir/barony"
probe="$build_dir/party_cross_map_live_probe"
if [[ ! -x $barony || ! -x $probe ]]
then
	echo "$build_dir/barony and party_cross_map_live_probe must exist" >&2
	exit 2
fi
if ! command -v bwrap >/dev/null 2>&1
then
	echo "bubblewrap is required for the isolated live runner" >&2
	exit 2
fi

run_dir=$(mktemp -d "$build_dir/party-live-acceptance.XXXXXX")
output_dir="$run_dir/output"
command_fifo="$run_dir/admin.fifo"
mkdir -p "$output_dir/maps"
if [[ ! -f $data_dir/maps/start.lmp ]]
then
	echo "live data directory has no maps/start.lmp fixture" >&2
	exit 2
fi
# A renamed copy of the known multiplayer-safe start map provides a distinct
# map identity with valid Player Starts, independent of optional mod content.
cp -- "$data_dir/maps/start.lmp" "$output_dir/maps/partyprobe.lmp"
mkfifo "$command_fifo"
exec 3<>"$command_fifo"

server_pid=
probe_pid=
cleanup()
{
	if [[ -n ${probe_pid:-} ]] && kill -0 "$probe_pid" 2>/dev/null
	then
		kill -TERM "$probe_pid" 2>/dev/null || true
		wait "$probe_pid" 2>/dev/null || true
	fi
	if [[ -n ${server_pid:-} ]] && kill -0 "$server_pid" 2>/dev/null
	then
		printf 'shutdown\n' >&3 || true
		for unused in {1..100}
		do
			kill -0 "$server_pid" 2>/dev/null || break
			sleep 0.1
		done
		if kill -0 "$server_pid" 2>/dev/null
		then
			kill -TERM "$server_pid" 2>/dev/null || true
		fi
		wait "$server_pid" 2>/dev/null || true
	fi
	exec 3>&- || true
	echo "party live acceptance artifacts: $run_dir"
}
trap cleanup EXIT INT TERM

wait_for_log()
{
	local log_file=$1
	local pattern=$2
	local description=$3
	for unused in {1..900}
	do
		if grep -q "$pattern" "$log_file" 2>/dev/null
		then
			return 0
		fi
		if [[ -n ${probe_pid:-} ]] && ! kill -0 "$probe_pid" 2>/dev/null
		then
			wait "$probe_pid" || true
			probe_pid=
			echo "$description failed because the probe exited" >&2
			tail -n 100 "$log_file" >&2 || true
			return 1
		fi
		if [[ -n ${server_pid:-} ]] && ! kill -0 "$server_pid" 2>/dev/null
		then
			wait "$server_pid" || true
			server_pid=
			echo "$description failed because the server exited" >&2
			return 1
		fi
		sleep 0.1
	done
	echo "timed out waiting for $description" >&2
	tail -n 100 "$log_file" >&2 || true
	return 1
}

start_server()
{
	local server_log=$1
	(
		cd "$data_dir"
		export BARONY_AUTOMATIA_LIVE_PROBE=1
		exec bwrap \
			--ro-bind / / \
			--bind "$output_dir" /home/conner/.barony \
			--dev-bind /dev /dev \
			--proc /proc \
			--tmpfs /tmp \
			"$barony" \
			--headless \
			--LAN \
			"--port=$port" \
			--server-name=AutomatiaPartyAcceptance \
			--autostart \
			--late-join \
			--character-save=local \
			--save=99
	) <&3 >"$server_log" 2>&1 &
	server_pid=$!
	wait_for_log \
		"$server_log" \
		"HEADLESS SERVER: autostarting game" \
		"headless gameplay startup"
	wait_for_log \
		"$server_log" \
		"LoadMap .*start.lmp" \
		"initial start map"
}

server_log_one="$run_dir/server-phase-one.log"
probe_log_one="$run_dir/probe-phase-one.log"
start_server "$server_log_one"

"$probe" "$port" >"$probe_log_one" 2>&1 &
probe_pid=$!
wait_for_log "$probe_log_one" "PARTY_ESTABLISHED" "party establishment"
wait_for_log "$probe_log_one" "SPOOF_REJECTION_OK" "spoof rejection"

source_slot=$(sed -n \
	's/.*PARTY_ESTABLISHED source=\([0-9][0-9]*\).*/\1/p' \
	"$probe_log_one" | tail -n 1)
transition_slot=$(sed -n \
	's/.*transition=\([0-9][0-9]*\).*/\1/p' \
	"$probe_log_one" | tail -n 1)
party_id=$(sed -n \
	's/.*PARTY_ESTABLISHED.*party=\([0-9][0-9]*\).*/\1/p' \
	"$probe_log_one" | tail -n 1)
if [[ -z $source_slot || -z $transition_slot || -z $party_id ]]
then
	echo "could not parse party probe coordination fields" >&2
	exit 1
fi

printf 'party-probe-transition %s partyprobe.lmp\n' "$transition_slot" >&3
wait_for_log \
	"$server_log_one" \
	"Player $transition_slot transitioned independently from .* to 'partyprobe.lmp#world'" \
	"independent divergent-map transition"
wait_for_log "$probe_log_one" "CROSS_MAP_PARTY_OK" "cross-map party synchronization"
wait_for_log "$probe_log_one" "SOCIAL_UI_CROSS_MAP_OK" "cross-map Social projection"
wait_for_log "$probe_log_one" "PARTY_CHAT_CROSS_MAP_OK" "cross-map Party chat"
printf 'party-probe-entity %s\n' "$source_slot" >&3

if ! wait "$probe_pid"
then
	probe_pid=
	tail -n 120 "$probe_log_one" >&2
	tail -n 160 "$server_log_one" >&2
	exit 1
fi
probe_pid=
if ! grep -q "party cross-map live probe passed" "$probe_log_one"
then
	echo "cross-map probe did not report success" >&2
	exit 1
fi
if ! grep -q "SOCIAL_UI_STATE_OK create_invite_accept" "$probe_log_one"
then
	echo "live probe did not validate the initial Social flow" >&2
	exit 1
fi
if ! grep -q "GLOBAL_CHAT_UNCHANGED_OK" "$probe_log_one" \
	|| ! grep -q "PARTYLESS_CHAT_REJECTION_OK" "$probe_log_one" \
	|| ! grep -q "PARTY_CHAT_SPOOF_REJECTION_OK" "$probe_log_one"
then
	echo "live probe did not validate Party-chat authority boundaries" >&2
	exit 1
fi
if ! grep -q "Player $transition_slot transitioned independently" "$server_log_one"
then
	echo "server did not record the independent map transition" >&2
	exit 1
fi
if ! grep -q "emitted marked ENTU" "$server_log_one"
then
	echo "server did not emit the scoped entity acceptance record" >&2
	exit 1
fi

printf 'shutdown\n' >&3
wait "$server_pid"
server_pid=
if ! grep -q "success$" "$server_log_one"
then
	echo "phase-one server did not complete clean shutdown" >&2
	exit 1
fi

world_save=$(find "$output_dir" -type f \
	-name '*.automatia-world.json' -print -quit)
if [[ -z $world_save || ! -f $world_save ]]
then
	echo "phase-one shutdown did not create an Automatia world save" >&2
	exit 1
fi
if ! grep -q "\"id\": $party_id" "$world_save"
then
	echo "saved world does not contain durable PartyID $party_id" >&2
	exit 1
fi

server_log_two="$run_dir/server-phase-two.log"
probe_log_two="$run_dir/probe-phase-two.log"
start_server "$server_log_two"

"$probe" "$port" --verify-restored "$party_id" \
	>"$probe_log_two" 2>&1 &
probe_pid=$!
if ! wait "$probe_pid"
then
	probe_pid=
	tail -n 120 "$probe_log_two" >&2
	tail -n 160 "$server_log_two" >&2
	exit 1
fi
probe_pid=
if ! grep -q "restored party live probe passed: party=$party_id" \
	"$probe_log_two"
then
	echo "restart probe did not restore PartyID $party_id" >&2
	exit 1
fi
if ! grep -q "SOCIAL_UI_KICK_OK party=$party_id" "$probe_log_two"
then
	echo "restart probe did not validate authoritative Social kick refresh" >&2
	exit 1
fi
if ! grep -q "Hydrated 1 persistent party record" "$server_log_two"
then
	echo "restart server did not report persistent party hydration" >&2
	exit 1
fi

printf 'shutdown\n' >&3
wait "$server_pid"
server_pid=
if ! grep -q "success$" "$server_log_two"
then
	echo "phase-two server did not complete clean shutdown" >&2
	exit 1
fi

echo "party cross-map live acceptance runner passed (party $party_id)"
