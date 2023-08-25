#include "extension.h"
#include "ISDKHooks.h"
#include "hooks.h"

TransmitManager g_Transmit;
SMEXT_LINK(&g_Transmit);

IGameConfig* g_pGameConf = nullptr;
ISDKHooks*   g_pSDKHooks = nullptr;

#ifdef DEBUG
uint64_t g_u64EngineThreadId;
#endif

SH_DECL_MANUALHOOK2_void(SetTransmit, 0, 0, 0, CCheckTransmitInfo*, bool);

struct HookingEntity {
    HookingEntity(CBaseEntity* pEntity, bool defaultTransmit)
    {
        m_bBlockAll        = false;
        m_bRemoveFlags     = false;
        m_bDefaultTransmit = defaultTransmit;
        m_nOwnerEntity     = -1;
        m_iEntityIndex     = gamehelpers->EntityToBCompatRef(pEntity);
        m_iHookId          = SH_ADD_MANUALHOOK(
            SetTransmit, pEntity,
            SH_MEMBER(&g_Transmit, &TransmitManager::Hook_SetTransmit), false);

        // for css, gotv is 65
        for (auto i = 1; i <= SM_MAXPLAYERS; i++)
        {
            // can see by default
            m_bCanTransmit[i] = defaultTransmit;
        }

        auto* pName = gamehelpers->GetEntityClassname(pEntity);
        if (pName != nullptr && (V_strcasecmp(pName, "info_particle_system") == 0 || V_strcasecmp(pName, "light_dynamic") == 0 || V_strcasecmp(pName, "env_cascade_light") == 0 || V_strcasecmp(pName, "env_projectedtexture") == 0 || V_strcasecmp(pName, "env_screenoverlay") == 0 || V_strcasecmp(pName, "env_fog_controller") == 0 || V_strcasecmp(pName, "env_lightglow") == 0 || V_strcasecmp(pName, "env_particlesmokegrenade") == 0 || V_strcasecmp(pName, "env_global_light") == 0 || V_strcasecmp(pName, "env_sun") == 0 || V_strcasecmp(pName, "env_sprite") == 0 || V_strcasecmp(pName, "point_camera") == 0 || V_strcasecmp(pName, "point_viewproxy") == 0 || V_strcasecmp(pName, "inferno") == 0 || V_strcasecmp(pName, "sunshine_shadow_control") == 0 || V_strcasecmp(pName, "cfe_player_decal") == 0 || V_strcasecmp(pName, "func_precipitation") == 0 || V_strcasecmp(pName, "cs_ragdoll") == 0 || V_strcasecmp(pName, "info_target") == 0 || V_strncasecmp(pName, "point_viewcontrol", 17) == 0 || V_strncasecmp(pName, "env_fire", 8) == 0 || V_strncasecmp(pName, "color_correction", 16) == 0))
        {
            m_bRemoveFlags = true;

            auto* edict = gamehelpers->EdictOfIndex(m_iEntityIndex);
            if (edict)
            {
                const auto flags     = edict->m_fStateFlags;
                edict->m_fStateFlags = flags & (~FL_EDICT_ALWAYS);
            }
        }
    }

    ~HookingEntity()
    {
        if (m_iHookId)
        {
            SH_REMOVE_HOOK_ID(m_iHookId);
            m_iHookId = 0;
        }
    }

    bool CanSee(int client) const
    {
        // remove self bypass
        if (m_iEntityIndex == client)
        {
            // self or children
            return true;
        }

        return m_bCanTransmit[client];
    }

    void SetSee(int client, bool can)
    {
        m_bCanTransmit[client] = can;
    }

    void SetDefault(int client)
    {
        m_bCanTransmit[client] = m_bDefaultTransmit;
    }

    int GetOwner() const
    {
        return m_nOwnerEntity;
    }

    void SetOwner(int owner)
    {
        m_nOwnerEntity = owner;
    }

    bool ShouldRemove() const
    {
        return m_bRemoveFlags;
    }

    void SetBlockAll(bool state)
    {
        m_bBlockAll = state;
    }

    bool GetBlockAll() const
    {
        return m_bBlockAll;
    }

private:
    int  m_iHookId;
    int  m_iEntityIndex;
    bool m_bCanTransmit[SM_MAXPLAYERS + 1];
    int  m_nOwnerEntity;
    bool m_bRemoveFlags;
    bool m_bDefaultTransmit;
    bool m_bBlockAll;
};

HookingEntity* g_Hooked[MAX_EDICTS];

void OnGameFrame(bool simulating)
{
    g_Transmit.CheckHooks();
}

void TransmitManager::Hook_SetTransmit(CCheckTransmitInfo* pInfo,
                                       bool                bAlways)
{
#ifdef DEBUG
    const uint64_t threadId = GetCurrentThreadId();
    if (threadId != g_u64EngineThreadId)
        Msg("Hook_SetTransmit(%p, %p, %s) at %" PRIu64 " :: %" PRIu64 " \n", META_IFACEPTR(CBaseEntity), pInfo, bAlways ? "true" : "false", threadId, g_u64EngineThreadId);
#endif

    const auto entity = gamehelpers->EntityToBCompatRef(META_IFACEPTR(CBaseEntity));
    const auto client = gamehelpers->IndexOfEdict(pInfo->m_pClientEnt);

    if (!IsEntityIndexInRange(entity) || client == entity)
    {
        RETURN_META(MRES_IGNORED);
    }

    if (g_Hooked[entity] == nullptr)
    {
        smutils->LogError(myself, "Why Hooked Entity is nullptr <%d.%s>",
                          entity, gamehelpers->GetEntityClassname(META_IFACEPTR(CBaseEntity)));
        RETURN_META(MRES_IGNORED);
    }

    if (g_Hooked[entity]->ShouldRemove())
    {
        const auto pEdict = gamehelpers->EdictOfIndex(entity);
        if (!pEdict || pEdict->IsFree())
        {
            RETURN_META(MRES_IGNORED);
        }

        const auto flags = pEdict->m_fStateFlags;
        if (flags & FL_EDICT_ALWAYS)
        {
            pEdict->m_fStateFlags = (flags ^ FL_EDICT_ALWAYS);
        }
    }

    // if we need block it with owner stats = transmit
    if (!g_Hooked[entity]->CanSee(client))
    {
        // blocked
        RETURN_META(MRES_SUPERCEDE);
    }

    const auto owner = g_Hooked[entity]->GetOwner();
    if (owner == client)
    {
        // don't block children
        RETURN_META(MRES_IGNORED);
    }

    // if entity shouldn't transmit for others
    if (g_Hooked[entity]->GetBlockAll())
    {
        // blocked
        RETURN_META(MRES_SUPERCEDE);
    }

    if (IsEntityIndexInRange(owner) && g_Hooked[owner] != nullptr && !g_Hooked[owner]->CanSee(client))
    {
        // blocked
        RETURN_META(MRES_SUPERCEDE);
    }

    // META_CONPRINTF("Hook_SetTransmit-> %d | %d | %d | %s\n", entity, owner,
    // client, g_Hooked[entity]->CanSee(client) ? "true" : "false");

    RETURN_META(MRES_IGNORED);
}

bool TransmitManager::SDK_OnMetamodLoad(ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    g_OriginalSourceHook = g_SHPtr;
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    return true;
}

bool TransmitManager::SDK_OnLoad(char* error, size_t maxlength, bool late)
{
#ifdef DEBUG
    g_u64EngineThreadId = GetCurrentThreadId();
#endif

    sharesys->AddDependency(myself, "sdkhooks.ext", true, true);
    SM_GET_IFACE(SDKHOOKS, g_pSDKHooks);

    if (!gameconfs->LoadGameConfigFile("sdkhooks.games", &g_pGameConf, error, maxlength))
    {
        smutils->Format(error, maxlength, "Failed to load SDKHooks gamedata.");
        return false;
    }

    auto offset = -1;
    if (!g_pGameConf->GetOffset("SetTransmit", &offset))
    {
        smutils->Format(error, maxlength, "Failed to load 'SetTransmit' offset.");
        return false;
    }
    m_nOffsetVTable = offset;

    playerhelpers->AddClientListener(this);
    g_pSDKHooks->AddEntityListener(this);

    extern sp_nativeinfo_t g_Natives[];
    g_pShareSys->AddNatives(myself, g_Natives);
    g_pShareSys->RegisterLibrary(myself, "TransmitManager");

    sv_parallel_send = g_pCVar->FindVar("sv_parallel_send");
    if (sv_parallel_send != nullptr)
    {
        m_bLastFrameState = sv_parallel_send->GetBool();

        g_SHPtr = g_ParallelSourceHook;

        smutils->LogMessage(myself, "Source Hook initialized as parallel mode!");
    }

    ResetHooks();

    smutils->AddGameFrameHook(&OnGameFrame);

    return true;
}

void TransmitManager::ResetHooks()
{
    for (auto i = 0; i < MAX_EDICTS; i++)
    {
        if (g_Hooked[i] != nullptr)
        {
            delete g_Hooked[i];
        }
        g_Hooked[i] = nullptr;
    }

    SH_MANUALHOOK_RECONFIGURE(SetTransmit, m_nOffsetVTable, 0, 0);
}

void TransmitManager::CheckHooks()
{
    if (sv_parallel_send == nullptr || sv_parallel_send->GetBool() == m_bLastFrameState)
        return;

    m_bLastFrameState = sv_parallel_send->GetBool();

    // remove all hooks
    ResetHooks();

    if (sv_parallel_send->GetBool())
    {
        g_SHPtr = g_ParallelSourceHook;
    }
    else
    {
        g_SHPtr = g_OriginalSourceHook;
    }

    // configure new hooks
    ResetHooks();

    smutils->LogMessage(myself, "sv_parallel_send state changed! current: %s", sv_parallel_send->GetBool() ? "true" : "false");
}

void TransmitManager::NotifyInterfaceDrop(SMInterface* pInterface)
{
    if (strcmp(pInterface->GetInterfaceName(), SMINTERFACE_SDKHOOKS_NAME) == 0)
    {
        g_pSDKHooks = nullptr;
    }
}

bool TransmitManager::QueryRunning(char* error, size_t maxlength)
{
    SM_CHECK_IFACE(SDKHOOKS, g_pSDKHooks)
    return true;
}

void TransmitManager::SDK_OnUnload()
{
    playerhelpers->RemoveClientListener(this);

    // I don't know why SDKHooks dropped first.
    if (g_pSDKHooks != nullptr)
    {
        g_pSDKHooks->RemoveEntityListener(this);
    }

    for (auto i = 0; i < MAX_EDICTS; i++)
    {
        if (g_Hooked[i] != nullptr)
        {
            delete g_Hooked[i];
        }
    }
}

void TransmitManager::OnEntityDestroyed(CBaseEntity* pEntity)
{
    auto entity = gamehelpers->EntityToBCompatRef(pEntity);

    if ((unsigned)entity == INVALID_EHANDLE_INDEX || (entity > 0 && entity <= playerhelpers->GetMaxClients()))
    {
        // This can be -1 for player entity before any players have connected
        return;
    }

    if (!IsEntityIndexInRange(entity))
    {
        // out-of-range
        return;
    }

    UnhookEntity(entity);
}

void TransmitManager::OnClientPutInServer(int client)
{
    const auto pPlayer = playerhelpers->GetGamePlayer(client);
    if (!pPlayer || pPlayer->IsSourceTV() || pPlayer->IsReplay())
    {
        return;
    }

    for (auto i = 1; i < MAX_EDICTS; i++)
    {
        if (g_Hooked[i] == nullptr)
        {
            // not being hook
            continue;
        }

        g_Hooked[i]->SetDefault(client);
    }
}

void TransmitManager::OnClientDisconnecting(int client)
{
    const auto pPlayer = playerhelpers->GetGamePlayer(client);
    if (!pPlayer || pPlayer->IsSourceTV() || pPlayer->IsReplay() || !pPlayer->IsInGame())
    {
        // not in-game = no hook
        return;
    }

    UnhookEntity(client);
}

void TransmitManager::HookEntity(CBaseEntity* pEntity, bool defaultTransmit)
{
    auto index = gamehelpers->EntityToBCompatRef(pEntity);

    if (!IsEntityIndexInRange(index))
    {
        // out-of-range
        smutils->LogError(myself, "Failed to hook entity %d -> out-of-range.", index);
        return;
    }

    if (g_Hooked[index] != nullptr)
    {
        // smutils->LogError(myself, "Entity Hook listener [%d] is not nullptr. Try to remove.", index);
        // delete g_Hooked[index];
    }

    g_Hooked[index] = new HookingEntity(pEntity, defaultTransmit);
    // smutils->LogMessage(myself, "Hooked entity %d", index);
}

void TransmitManager::UnhookEntity(int index)
{
    if (!IsEntityIndexInRange(index))
    {
        // out-of-range
        smutils->LogError(myself, "Failed to unhook entity %d -> out-of-range.", index);
        return;
    }

    if (g_Hooked[index] == nullptr)
    {
        // smutils->LogError(myself, "Entity Hook listener %d is nullptr.
        // Skipped.", index);
        return;
    }

    delete g_Hooked[index];
    g_Hooked[index] = nullptr;
    // smutils->LogMessage(myself, "Unhooked entity %d", index);
}

static cell_t Native_SetEntityOwner(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    if (g_Hooked[params[1]] != nullptr)
    {
        g_Hooked[params[1]]->SetOwner(params[2]);
        return true;
    }

    if (params[1] >= 1 && params[1] < playerhelpers->GetMaxClients())
    {
        smutils->LogError(myself, "Entity %d is not being hook.", params[1]);
        return false;
    }

    return pContext->ThrowNativeError("Entity %d is not being hook.", params[1]);
}

static cell_t Native_SetEntityState(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    auto* pPlayer = playerhelpers->GetGamePlayer(params[2]);
    if (!pPlayer || !pPlayer->IsInGame())
    {
        return pContext->ThrowNativeError("Client %d is invalid.", params[2]);
    }

    if (g_Hooked[params[1]] != nullptr)
    {
        g_Hooked[params[1]]->SetSee(params[2], params[3]);
        return true;
    }

    if (params[1] >= 1 && params[1] < playerhelpers->GetMaxClients())
    {
        smutils->LogError(myself, "Entity %d is not being hook.", params[1]);
        return false;
    }

    return pContext->ThrowNativeError("Entity %d is not being hook.", params[1]);
}

static cell_t Native_AddEntityHooks(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    auto* pEntity = gamehelpers->ReferenceToEntity(params[1]);
    if (!pEntity)
    {
        // nuull
        return pContext->ThrowNativeError("Entity %d is invalid.", params[1]);
    }

    bool defaultTransmit = true;
    if (params[0] >= 2)
        defaultTransmit = !!params[2];

    g_Transmit.HookEntity(pEntity, defaultTransmit);

    return 0;
}

static cell_t Native_RemoveEntHooks(IPluginContext* pContext, const cell_t* params)
{
    const auto index = params[1];

    if (!IsEntityIndexInRange(index))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", index);
    }

    auto* pEntity = gamehelpers->ReferenceToEntity(index);
    if (!pEntity)
    {
        // nuull
        return pContext->ThrowNativeError("Entity %d is invalid.", index);
    }

    if (g_Hooked[index] != nullptr)
    {
        delete g_Hooked[index];
    }

    return 0;
}

static cell_t Native_GetEntityState(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    auto* pPlayer = playerhelpers->GetGamePlayer(params[2]);
    if (!pPlayer || !pPlayer->IsInGame())
    {
        return pContext->ThrowNativeError("Client %d is invalid.", params[2]);
    }

    if (g_Hooked[params[1]] == nullptr)
    {
        // can see
        return true;
    }

    return g_Hooked[params[1]]->CanSee(params[2]);
}

static cell_t Native_GetEntityBlock(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    if (g_Hooked[params[1]] == nullptr)
    {
        // can see
        return false;
    }

    return g_Hooked[params[1]]->GetBlockAll();
}

static cell_t Native_SetEntityBlock(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    if (g_Hooked[params[1]] == nullptr)
    {
        // can see
        return false;
    }

    g_Hooked[params[1]]->SetBlockAll(!!params[2]);
    return true;
}

static cell_t Native_IsEntityHooked(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    return g_Hooked[params[1]] != nullptr;
}

sp_nativeinfo_t g_Natives[] = {
    {"TransmitManager_AddEntityHooks", Native_AddEntityHooks},
    {"TransmitManager_RemoveEntHooks", Native_RemoveEntHooks},
    {"TransmitManager_SetEntityOwner", Native_SetEntityOwner},
    {"TransmitManager_SetEntityState", Native_SetEntityState},
    {"TransmitManager_GetEntityState", Native_GetEntityState},
    {"TransmitManager_SetEntityBlock", Native_SetEntityBlock},
    {"TransmitManager_GetEntityBlock", Native_GetEntityBlock},
    {"TransmitManager_IsEntityHooked", Native_IsEntityHooked},
    {nullptr,                          nullptr              },
};
