# Module Brain Addendum

Module Name: mod-playerbots
Module Version: 2.3.8a-hotfix1
RTG Brain Compatibility Version: 5.4.0
Commit Title: Restore RTG queue penalty helper symbol
Commit Description: Adds the missing local RTG_ClearQueuePenalties helper in BattleGroundJoinAction.cpp so BG leave-block and join cleanup code compiles again while preserving the intended deserter-penalty stripping behavior.

--------------------------------

## Module Purpose

Provides RTG queue-assistance behavior for battleground and dungeon helpers inside mod-playerbots.

--------------------------------

## Architecture Overview

- Battleground join/leave actions
- RTG helper ownership and pending-state checks
- Queue penalty cleanup before or during protected helper transitions

--------------------------------

## Runtime Control Path

RTG helper state
→ battleground join/leave action
→ queue penalty cleanup
→ queue / leave decision

--------------------------------

## Data Structures

- RTG bot event cache values
- battleground queue assignment string in add event data

--------------------------------

## Config Interface

Uses existing RTG event-driven queue-assistance configs already present in the module.

--------------------------------

## Database Structures

Module currently uses no DB persistence in this hotfix.

--------------------------------

## Integration Points

- BattleGroundJoinAction
- RandomPlayerbotMgr
- RTG queue event cache

--------------------------------

## Lifecycle Model

This hotfix does not change lifecycle rules. It restores a missing helper used during queue penalty cleanup.

--------------------------------

## Known Constraints

- Depends on aura IDs present on this branch
- Intended only as a compile/runtime hotfix for the missing helper symbol

--------------------------------

## Future Evolution Hooks

- Centralize RTG queue penalty cleanup so BG and LFG share one helper source cleanly

--------------------------------

## Files Modified In This Revision

- src/Ai/Base/Actions/BattleGroundJoinAction.cpp
- brain_addendum/mod-playerbots_brain_addendum.md

--------------------------------

## Behavioral Changes In This Revision

No intended gameplay redesign. Restores deserter-penalty clearing calls in BG action paths by defining the missing helper.

--------------------------------

## Test Plan

- Rebuild the module
- Confirm BattleGroundJoinAction.cpp compiles
- Queue into a battleground with RTG helpers enabled
- Confirm helpers still join and no undefined-symbol compile errors remain

--------------------------------

## Notes For RTG Brain Ingestion

This revision is a focused compile hotfix. Earlier logic already referenced RTG_ClearQueuePenalties; the symbol had simply dropped out of the current file state.
