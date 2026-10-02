update hm_fetchaccounts set fadaystokeep = -2 where faservertype = 1 and fadaystokeep < 0;

update hm_fetchaccounts set fadaystokeep = -1 where faservertype = 1 and fadaystokeep = 0;

update hm_fetchaccounts set fadaystokeep = 0 where faservertype = 1 and fadaystokeep = -2;

update hm_fetchaccounts set fadaystokeep = 0 where faservertype = 0 and faorigin <> 0 and fadaystokeep < 0;

update hm_dbversion set value = 6106;
