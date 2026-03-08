# RTG Role/Context Framework Roadmap

This module snapshot now includes the earlier healer/role work plus the concrete queue-handling rules from **V5** and **V6**. The purpose of this document is to keep the project from drifting backward on later passes.

## Locked goals
These are now treated as concrete goals, not loose ideas.

### Queue handling
- RDF and BG queue checks must continue running while `rtgEventDriven` is enabled.
- BG support must be serviced on a short cadence instead of being effectively disabled in RTG mode.
- RDF support should react on the same short cadence once the grace window expires.
- Pending queue reservations must expire quickly enough that a filled first dungeon does not cause the next dungeon queue to go stale behind it.
- Mixed parties that contain real players must not have random bots spamming role checks or restarting queue flow on their own.

### Battleground behavior
- AB, WSG, and EotS are first-class test targets.
- A bot explicitly brought online for BG support must be allowed to queue even if legacy `activeBgQueue` / `bgInstanceCount` counters have not caught up yet.
- BG helper failures must be diagnosable from logs instead of remaining silent.

### RDF behavior
- RDF role choice must continue matching actual spec.
- Healer-role dungeon bots should remain healing-first.
- The queue system should favor fast recycling of stale helper reservations over leaving later queues blocked.

## V5 concrete changes
### Battleground scheduling
- `RandomPlayerbotMgr.cpp`
  - `CheckBgQueue()` now continues running when `rtgEventDriven` is enabled.
  - RTG mode uses a short 5-second BG service interval instead of effectively skipping BG support.

### Battleground join path
- `BattleGroundJoinAction.cpp`
  - RTG-assigned BG helpers are now allowed through `shouldJoinBg()` even when old BG counters have not updated yet.
  - The same allowance is mirrored in `FreeBGJoinAction::shouldJoinBg()`.

### Battleground stale reservation cleanup
- `RandomPlayerbotMgr.cpp`
  - `rtg_bg_pending` reservations now recycle on a short 15-second window instead of sitting around much longer.

## V6 concrete changes
### Faster RDF service cadence
- `RandomPlayerbotMgr.cpp`
  - `CheckLfgQueue()` now runs on a short cadence in RTG mode so RDF help does not wait for long scheduler gaps.

### Faster LFG stale reservation cleanup
- `RandomPlayerbotMgr.cpp`
  - `rtg_lfg_pending` reservations now recycle on a short 15-second window.

### Mixed-party anti-spam guard
- `LfgActions.cpp`
  - If a bot is grouped with any real player, it must not initiate or re-initiate queue flow on its own.
  - Real players control requeue and role-check flow in mixed parties.

### Grace defaults
- `PlayerbotAIConfig.cpp`
- `PlayerbotAIConfig.h`
- `conf/playerbots.conf.dist`
  - The default `AiPlayerbot.RTG.QueueGraceSeconds` baseline is now 30 seconds.

## Logging requirements going forward
Future passes should preserve or expand logging, not reduce it.

### Required queue logs
- BG demand detected for AB / WSG / EotS
- BG helper assigned
- BG helper allowed into `shouldJoinBg()`
- BG helper failed to queue and why
- LFG helper assigned
- LFG helper rejected because role/spec mismatched
- Pending reservation expired and was recycled

### Why this matters
We now know silent failures waste entire test rounds. Logging must keep queue-path failures visible.

## Current test focus
1. **Solo RDF**
   - queue as DPS
   - verify tank/healer/DPS helpers arrive quickly after grace expires

2. **Back-to-back RDF**
   - fill one dungeon
   - queue another immediately
   - verify the second queue is serviced quickly instead of stalling behind stale reservations

3. **Mixed-party RDF**
   - two real players in one party
   - verify bots stop spamming role checks and do not force requeue

4. **Solo BG**
   - AB
   - WSG
   - EotS
   - verify bots log in and actually queue

5. **RDF + BG simultaneously**
   - verify BG is still serviced while dungeon filling is happening

## Next concrete direction
The next passes should stay focused on:
- faster burst-style helper login once grace expires
- fair servicing of multiple simultaneous RDF queues
- BG helper visibility and failure logging
- no regression on healer-role dungeon behavior
