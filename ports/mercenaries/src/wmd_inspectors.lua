-- Appended only to the verified retail NW Allies 1 script, independent of OG Bugs.
-- These locals share that script's scope; no other mission is modified.
local wmdActive = false
local wmdTimer = nil
local wmdDead = {}
local wmdDelivered = {}
local wmdAtPost = {}
local wmdEnteringHQ = false
local retailInspectorDelivered = OnInspectorDelivered

local function wmdName(actor)
    if not tActor.Inspector then return nil end
    for _, name in pairs(tActor.Inspector) do
        if actor == name then return name end
    end
    local id = actor and Utility_GetActorAsInt(actor)
    if not id or id == 0 then return nil end
    for _, name in pairs(tActor.Inspector) do
        if id == Utility_GetActorAsInt(name) then return name end
    end
end

local function wmdStop()
    wmdActive = false
    wmdTimer = Event_CancelEvent(wmdTimer)
end

local function wmdRefreshInspection()
    local handle = tHUD[sCurrentSite]
    local tray = handle and HUD_GetTrayDisplay(handle)
    if not tray or not tray[1] then return end
    local count = 0
    for name, site in pairs(wmdAtPost) do
        if site == sCurrentSite and not wmdDead[name] and Actor_IsAlive(name) then
            count = count + 1
        end
    end
    nNumInspectorsAtPost = count
    tray[1].nDelta = nInspectDelta * count
    HUD_SetTrayDisplay(handle, tray)
end

function OnInspectorDelivered(actor, complete, unattached, objective, location, alive)
    if alive then
        return retailInspectorDelivered(actor, complete, unattached, objective, location, alive)
    end
    local name = wmdName(actor)
    if not wmdActive or not name or wmdDead[name] or wmdDelivered[name] then return end
    wmdDead[name] = true
    wmdAtPost[name] = nil
    if wmdEnteringHQ then
        table_DeleteElement(tAliveInspectors, name, true)
        nNumInspectors = table.getn(tAliveInspectors)
        if nNumInspectors == 0 then
            sVOPlayedOnFailure = tRadioMessage.TechInspectorDead
            mission_MissionFailure()
        else
            OnInspectorAtHQ_Done()
        end
    else
        retailInspectorDelivered(name, complete, unattached, objective, location, false)
    end
    wmdRefreshInspection()
    if nNumInspectors == 0 or (wmdEnteringHQ and nNumInspectorsInHQ == nNumInspectors) then wmdStop() end
end

-- A missed return-objective notification must also remove that target from
-- the delivery count, or the surviving inspectors could never finish returning.
local function wmdReportDeath(name)
    for _, target in pairs(mission_tTargets) do
        local objective = mission_tObjectives[target.objectiveName]
        if target.actorName == name and objective
            and (objective.callbackFunc == OnInspectorDelivered
                or objective.callbackFunc == OnInspectorDeliveredToHQ) then
            CancelEventsForObjectiveTarget(objective.verb, target)
            objective.targetsRemaining = objective.targetsRemaining - 1
            if objective.objectify then
                RemoveTargetActorBlip(target)
                if objective.targetsRemaining <= 0 then RemoveDestinationRegionBlip(target) end
            end
            objective.callbackFunc(name, objective.targetsRemaining <= 0, true,
                target.objectiveName, target.destLoc, false)
            return
        end
    end
    OnInspectorDelivered(name, false, true, tObjHandle.Escort, nil, false)
end

local retailTargetDestroyed
local function wmdTargetDestroyed(actor)
    local name = wmdName(actor)
    if name and (not wmdActive or wmdDead[name] or wmdDelivered[name]) then return end
    return retailTargetDestroyed(actor)
end

function Recomp_WmdInspectorsPoll()
    wmdTimer = Event_CancelEvent(wmdTimer)
    if not wmdActive then return end
    for _, name in pairs(tActor.Inspector) do
        if wmdActive and not wmdDead[name] and not wmdDelivered[name] and not Actor_IsAlive(name) then
            wmdReportDeath(name)
        end
    end
    if wmdActive then
        wmdTimer = Event_RelativeTimer("Recomp_WmdInspectorsPoll", "b", 0.25)
    end
end

local retailNotifications = post_InitialNotifications
function post_InitialNotifications()
    -- ScriptInit loads the shared objective functions after this chunk executes.
    retailTargetDestroyed = TargetActorDestroyed
    TargetActorDestroyed = wmdTargetDestroyed
    retailNotifications()
    wmdActive = true
    Recomp_WmdInspectorsPoll()
end

local retailStartInspection = StartInspection
function StartInspection(site, inspector)
    if not wmdActive or wmdDead[inspector] or not Actor_IsAlive(inspector) then return end
    if site ~= sCurrentSite then wmdAtPost = {} end
    retailStartInspection(site, inspector)
    wmdRefreshInspection()
end

local retailAtPost = OnInspecterAtPost
function OnInspecterAtPost(inspector, location, range, inside, threeD)
    local name = wmdName(inspector)
    local handle = tHUD[sCurrentSite]
    local tray = handle and HUD_GetTrayDisplay(handle)
    if not wmdActive or bSiteAborted or not name or wmdDead[name] or not Actor_IsAlive(name)
        or not tray or not tray[1] or wmdAtPost[name] == sCurrentSite then return end
    -- Discard arrivals queued for a previous site.
    if not tLocation[sCurrentSite]
        or Utility_GetActorAsInt(location) ~= Utility_GetActorAsInt(tLocation[sCurrentSite][name]) then return end
    wmdAtPost[name] = sCurrentSite
    retailAtPost(inspector, location, range, inside, threeD)
    wmdRefreshInspection()
end

local retailInspectionComplete = OnInspectionComplete
function OnInspectionComplete(handle, state)
    if state == HUD.STATE.LIMIT then wmdAtPost = {} end
    retailInspectionComplete(handle, state)
end

local retailCancelInspection = CancelInspection
function CancelInspection(site)
    if not site then return end
    -- Retail initializes this GUID-keyed record only after arrival, but its
    -- cancellation loop assumes every inspector has already reached a post.
    for _, name in pairs(tActor.Inspector) do
        local id = Utility_GetActorAsInt(name)
        if id and type(tEvent[id]) ~= "table" then tEvent[id] = {} end
    end
    retailCancelInspection(site)
    if site then
        tHUD[site] = nil
        wmdAtPost = {}
    end
end

local retailDeliveredToHQ = OnInspectorDeliveredToHQ
function OnInspectorDeliveredToHQ(actor, complete, unattached, objective, location, alive)
    if not alive then OnInspectorDelivered(actor, complete, unattached, objective, location, false) end
    if not wmdActive then return end
    if complete then wmdEnteringHQ = true end
    retailDeliveredToHQ(actor, complete, unattached, objective, location, true)
end

local retailEnterHQ = InspectorEntersHQ
function InspectorEntersHQ(actor)
    local name = wmdName(actor)
    if not wmdActive or not name or wmdDead[name] or wmdDelivered[name] or not Actor_IsAlive(name) then return end
    retailEnterHQ(actor)
end

local retailAtHQ = OnInspectorAtHQ
function OnInspectorAtHQ(inspector, location, distance, less, threeD)
    local name = wmdName(inspector)
    if not wmdActive or not name or wmdDead[name] or wmdDelivered[name] then return end
    if not Actor_IsAlive(name) then
        OnInspectorDelivered(name, false, true, tObjHandle.Return, nil, false)
        return
    end
    -- Arrival intentionally removes the actor; the monitor must not count it as dead.
    wmdDelivered[name] = true
    retailAtHQ(inspector, location, distance, less, threeD)
    if nNumInspectorsInHQ == nNumInspectors then wmdStop() end
end

function OnInspectorAtHQDied(inspector)
    OnInspectorDelivered(inspector, false, true, tObjHandle.Return, nil, false)
end

local retailCleanup = pre_MissionCleanup
function pre_MissionCleanup()
    wmdStop()
    retailCleanup()
end
