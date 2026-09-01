# Custom dialogue shared quests: two-client acceptance

This manual check complements the deterministic `quest_ownership` test. It
requires one authoritative server and two real game clients because automated
state tests cannot prove journal rendering, packet delivery, or disconnect UI.

## Test content

Create or explicitly upgrade three schema-2 dialogue quests in Zed:

- `scope_manual_personal`, ownership **Personal**
- `scope_manual_party`, ownership **Party**
- `scope_manual_world`, ownership **World**

Give each quest an accept action, a stage-advance action, one objective, a
reset action with `repeatable: true`, and an actor-only gold reward. Put the
World quest's second interaction in a different MapInstance or playable floor.

## Server and clients

1. Start a dedicated/headless server with the test world and persistent world
   save enabled.
2. Connect **Client A** and **Client B** using two different durable character
   identities.
3. On Client A, accept the Personal quest. Verify Client B cannot see or
   advance it.
4. Form one Party containing Client A and Client B. On Client A, accept and
   advance the Party quest. Verify Client B immediately sees the same stage and
   objective, without talking to the quest giver.
5. Have Client B leave the Party (repeat once using a leader kick). Advance the
   Party quest on Client A. Verify Client B no longer sees the Party state or
   receives later progress. Rejoin and verify the existing progress returns.
6. Advance the World quest on Client A. Move Client B to the divergent
   MapInstance/playable floor and verify Client B sees the same World stage.
   Matching X/Y coordinates must not affect either result.
7. Trigger the shared quest's gold reward as Client A. Verify Client A receives
   it once and Client B receives no duplicate reward.
8. Disconnect and reconnect Client B. Verify its late-join journal contains its
   Personal state, current Party state, and World state.
9. Save and stop the server, then restart from the same world save. Verify the
   same PartyID and Party/World progress return.
10. Disband the Party, create an unrelated new Party, and verify the new Party
    does not inherit the archived quest progress.

Record server logs for the authenticated identity binding, PartyID, QSBG story
sync, save transaction, and restart. A passing run requires both clients to
agree visually with the server-owned state; deterministic tests alone do not
claim this manual coverage.
