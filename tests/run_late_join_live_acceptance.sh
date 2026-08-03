#!/usr/bin/env bash

set -euo pipefail

usage()
{
	cat <<'EOF'
usage:
  run_late_join_live_acceptance.sh <output-dir> <save-slot> <port> new
  run_late_join_live_acceptance.sh <output-dir> <save-slot> <port> returning <player-slot>

The output directory must be inside the maindev worktree and contain the
prepared Barony output/save fixture. The runner redirects ~/.barony into that
directory, starts the FMOD headless LAN server, executes the UDP live probe,
requests diagnostic status, and shuts the server down cleanly.
EOF
}

if [[ $# -ne 4 && $# -ne 5 ]]
then
	usage >&2
	exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)
build_dir="$repo_root/build"
output_dir=$(realpath -e -- "$1")
save_slot=$2
port=$3
join_mode=$4

case "$output_dir/" in
	"$repo_root/"*) ;;
	*)
		echo "output directory must be inside $repo_root" >&2
		exit 2
		;;
esac

if [[ ! $save_slot =~ ^[0-9]+$ || $save_slot -gt 99 ]]
then
	echo "save slot must be between 0 and 99" >&2
	exit 2
fi
if [[ ! $port =~ ^[0-9]+$ || $port -lt 1 || $port -gt 65535 ]]
then
	echo "port must be between 1 and 65535" >&2
	exit 2
fi
if [[ $join_mode != new && $join_mode != returning ]]
then
	echo "join mode must be 'new' or 'returning'" >&2
	exit 2
fi
if [[ $join_mode == returning ]]
then
	if [[ $# -ne 5 || ! $5 =~ ^[0-9]+$ || $5 -lt 1 || $5 -gt 15 ]]
	then
		echo "returning mode requires a player slot from 1 through 15" >&2
		exit 2
	fi
	player_slot=$5
elif [[ $# -ne 4 ]]
then
	usage >&2
	exit 2
fi

barony="$build_dir/barony"
probe="$build_dir/late_join_live_probe"
save_file="$output_dir/savegames/host/savegame${save_slot}_mp.baronysave"
if [[ ! -x $barony || ! -x $probe ]]
then
	echo "build/barony and build/late_join_live_probe must exist" >&2
	exit 2
fi
if [[ ! -f $save_file ]]
then
	echo "missing prepared save fixture: $save_file" >&2
	exit 2
fi
if ! command -v bwrap >/dev/null 2>&1
then
	echo "bubblewrap is required to keep runtime writes inside maindev" >&2
	exit 2
fi

json_u32()
{
	local key=$1
	local value
	value=$(sed -n \
		"s/^[[:space:]]*\"${key}\":[[:space:]]*\([0-9][0-9]*\),.*$/\1/p" \
		"$save_file" | head -n 1)
	if [[ -z $value ]]
	then
		echo "could not read $key from $save_file" >&2
		exit 2
	fi
	printf '%s' "$value"
}

map_seed=$(json_u32 mapseed)
game_key=$(json_u32 gamekey)
lobby_key=$(json_u32 lobbykey)
reconnect_token=
if [[ $join_mode == returning ]]
then
	token_line=$((player_slot + 1))
	reconnect_token=$(sed -n \
		's/^[[:space:]]*"reconnect_token":[[:space:]]*"\([0-9a-f]*\)".*$/\1/p' \
		"$save_file" | sed -n "${token_line}p")
	if [[ ! $reconnect_token =~ ^[0-9a-f]{32}$ ]]
	then
		echo "save fixture has no valid reconnect token for player $player_slot" >&2
		exit 2
	fi
fi

run_dir=$(mktemp -d "$build_dir/late-join-acceptance.XXXXXX")
command_fifo="$run_dir/admin.fifo"
runner_log="$run_dir/server-console.log"
mkfifo "$command_fifo"
exec 3<>"$command_fifo"

server_pid=
cleanup()
{
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
	echo "server console log: $runner_log"
}
trap cleanup EXIT INT TERM

(
	cd "$build_dir"
	exec bwrap \
		--ro-bind / / \
		--bind "$output_dir" /home/conner/.barony \
		--dev-bind /dev /dev \
		--proc /proc \
		--tmpfs /tmp \
		./barony \
		--headless \
		--LAN \
		"--port=$port" \
		--server-name=AutomatiaLateJoinAcceptance \
		--autostart \
		--late-join \
		"--save=$save_slot"
) <&3 >"$runner_log" 2>&1 &
server_pid=$!

ready=false
for unused in {1..1200}
do
	if grep -q "HEADLESS SERVER: autostarting game" "$runner_log" \
		&& grep -q "LoadMap .*start.lmp" "$runner_log"
	then
		ready=true
		break
	fi
	if ! kill -0 "$server_pid" 2>/dev/null
	then
		echo "headless server exited before gameplay startup" >&2
		tail -n 80 "$runner_log" >&2
		exit 1
	fi
	sleep 0.1
done
if [[ $ready != true ]]
then
	echo "timed out waiting for headless gameplay startup" >&2
	tail -n 80 "$runner_log" >&2
	exit 1
fi

if [[ $join_mode == returning ]]
then
	"$probe" "$port" v5.0.2 "$player_slot" \
		"$map_seed" "$game_key" "$lobby_key" "$reconnect_token"
else
	"$probe" "$port" v5.0.2
fi

printf 'status\n' >&3
sleep 1
printf 'shutdown\n' >&3
wait "$server_pid"
server_pid=

if ! grep -q "HEADLESS INSTANCE:" "$runner_log"
then
	echo "headless status did not emit map-instance diagnostics" >&2
	exit 1
fi
if ! grep -q "success$" "$runner_log"
then
	echo "headless server did not complete clean shutdown" >&2
	exit 1
fi

echo "late-join live acceptance runner passed ($join_mode)"
