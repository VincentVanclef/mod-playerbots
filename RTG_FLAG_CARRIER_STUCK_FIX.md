# RTG Flag Carrier Stuck Fix

This build hardens flag-carrier movement in Eye of the Storm and Warsong Gulch.

## Fixed

- EotS carriers now route to the exact scoring area-trigger coordinates instead of only the visible tower/banner coordinate.
- EotS carriers retry movement after measured no-progress and use a tightly limited direct breadcrumb fallback when mmap rejects a hand-authored route segment.
- WSG carrier recovery now resets its watchdog timestamp after clearing movement. Previously, once the timeout was reached, the bot could clear its newly issued movement every AI pulse and remain frozen indefinitely.
- WSG carriers also receive the same limited breadcrumb fallback after repeated movement failures.
- Failed path validation can no longer return a silent "handled" result while issuing no movement at all.

## Install

Replace the current `mod-playerbots` module with this build, keep your live `playerbots.conf`, rebuild the core, and restart `worldserver`.

For a focused live test, temporarily enable:

```ini
AiPlayerbot.RTG.BG.DebugObjectiveBrain = 1
```

After confirming EotS carriers reach an owned tower and score, set it back to `0` to avoid extra log output.
