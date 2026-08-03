#!/usr/bin/env bash
set -u

cd "${1:-$HOME/maindev}" || exit 1

report="15player-foundation-audit.txt"
{
    echo "Barony Automatia 15-player foundation audit"
    echo "Repository: $(pwd)"
    echo "Generated: $(date -Is)"
    echo

    echo "=== Active branch ==="
    git branch --show-current 2>/dev/null || true
    echo

    echo "=== MAXPLAYERS and packet-size definitions ==="
    grep -RniE '#define[[:space:]]+MAXPLAYERS|#define[[:space:]]+NET_PACKET_SIZE|BARONY_SUPER_MULTIPLAYER' \
        CMakeLists.txt src/CMakeLists.txt src/main.hpp src/game.hpp 2>/dev/null || true
    echo

    echo "=== Hard-coded player-sized declarations ==="
    grep -RniE \
        'lockedSlots\[[[:space:]]*(4|8)[[:space:]]*\]|players?\[[[:space:]]*(4|8)[[:space:]]*\]|client_.*\[[[:space:]]*(4|8)[[:space:]]*\]|stats\[[[:space:]]*(4|8)[[:space:]]*\]' \
        src --include='*.cpp' --include='*.hpp' 2>/dev/null || true
    echo

    echo "=== Loops that may assume four/eight network players ==="
    grep -RniE \
        'for[[:space:]]*\([^;]*;[^;]*(<|<=)[[:space:]]*(4|8)[[:space:]]*;' \
        src --include='*.cpp' --include='*.hpp' 2>/dev/null || true
    echo

    echo "=== Modulo-four player mappings requiring review ==="
    grep -RniE \
        '(player|playernum|clientnum|slot)[^;\n]*%[[:space:]]*4|%[[:space:]]*4[^;\n]*(player|slot)' \
        src --include='*.cpp' --include='*.hpp' 2>/dev/null || true
    echo

    echo "=== Fixed four-slot lobby signatures ==="
    grep -RniE 'lobbyPlayerJoinRequest|lockedSlots' \
        src --include='*.cpp' --include='*.hpp' 2>/dev/null || true
} | tee "$report"

echo
echo "Saved audit to: $(pwd)/$report"
