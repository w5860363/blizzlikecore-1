-- BlizzLikeCore TeleNPC
--
-- Remove TeleNPC
--

DELETE FROM creature WHERE guid BETWEEN 1550400 AND 1550408;
DELETE FROM creature_addon WHERE guid BETWEEN 1550400 AND 1550408;
DELETE FROM game_event_creature WHERE guid BETWEEN 1550400 AND 1550408;
DELETE FROM game_event_model_equip WHERE guid BETWEEN 1550400 AND 1550408;