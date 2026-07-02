-- RTG: queue-demand target levels now use the rounded average of real queued player levels.
-- Clear stale demand-prepared bot state so existing cached level-19/level-10 targets are recalculated.
DELETE FROM `playerbots_db_store`
WHERE `key` IN ('rtg_demand_mode', 'rtg_demand_role', 'rtg_demand_level', 'rtg_demand_spec', 'level')
  AND `guid` IN (
      SELECT `guid` FROM (
          SELECT DISTINCT `guid`
          FROM `playerbots_db_store`
          WHERE `key` IN ('rtg_demand_mode', 'rtg_demand_role', 'rtg_demand_level', 'rtg_demand_spec')
      ) AS `rtg_demand_guids`
  );
