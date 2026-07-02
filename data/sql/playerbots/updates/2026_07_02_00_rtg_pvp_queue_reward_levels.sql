-- RTG: clear stale queue-demand bot targets so old level-10 PvP filler bots
-- are rebuilt using the new bracket-cap/reward-level targeting rules.
DELETE FROM `playerbots_random_bots`
WHERE `event` IN ('rtg_demand_mode', 'rtg_demand_role', 'rtg_demand_level', 'rtg_demand_spec');
