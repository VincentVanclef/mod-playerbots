-- RTG: remove invalid weapon/shield/wand proficiency spells that older bot builds could learn.
-- This matches AzerothCore's race/class validation cleanup, but removes the bad rows up front
-- so existing bots stop logging invalid proficiency warnings on startup/load.

DELETE cs
FROM `character_spell` cs
JOIN `characters` ch ON ch.`guid` = cs.`guid`
WHERE
    (cs.`spell` = 196 AND ch.`class` NOT IN (1, 2, 3, 4, 6, 7)) OR       -- One-Handed Axes
    (cs.`spell` = 197 AND ch.`class` NOT IN (1, 2, 3, 6, 7)) OR          -- Two-Handed Axes
    (cs.`spell` = 198 AND ch.`class` NOT IN (1, 2, 4, 5, 6, 7, 11)) OR   -- One-Handed Maces
    (cs.`spell` = 199 AND ch.`class` NOT IN (1, 2, 6, 7, 11)) OR         -- Two-Handed Maces
    (cs.`spell` = 200 AND ch.`class` NOT IN (1, 2, 3, 6, 11)) OR         -- Polearms
    (cs.`spell` = 201 AND ch.`class` NOT IN (1, 2, 3, 4, 6, 8, 9)) OR    -- One-Handed Swords
    (cs.`spell` = 202 AND ch.`class` NOT IN (1, 2, 3, 6)) OR             -- Two-Handed Swords
    (cs.`spell` = 227 AND ch.`class` NOT IN (1, 3, 5, 7, 8, 9, 11)) OR   -- Staves
    (cs.`spell` = 264 AND ch.`class` NOT IN (1, 3, 4)) OR                -- Bows
    (cs.`spell` = 266 AND ch.`class` NOT IN (1, 3, 4)) OR                -- Guns
    (cs.`spell` = 5011 AND ch.`class` NOT IN (1, 3, 4)) OR               -- Crossbows
    (cs.`spell` = 2567 AND ch.`class` NOT IN (1, 3, 4)) OR               -- Thrown
    (cs.`spell` = 5009 AND ch.`class` NOT IN (5, 8, 9)) OR               -- Wands
    (cs.`spell` = 9116 AND ch.`class` NOT IN (1, 2, 7)) OR               -- Shields
    (cs.`spell` = 1180 AND ch.`class` NOT IN (1, 3, 4, 5, 7, 8, 9, 11)) OR -- Daggers
    (cs.`spell` = 15590 AND ch.`class` NOT IN (1, 3, 4, 7, 11));         -- Fist Weapons
