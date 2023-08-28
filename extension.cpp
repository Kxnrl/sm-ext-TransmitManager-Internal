#include "extension.h"
#include "CDetour/detours.h"
#include "ISDKHooks.h"
#include "macro.h"

#include <shared_mutex>

// #define DEBUG
// #define TRACE

constexpr int DEBUG_PROFILER_PRINT_MAIN_THREAD_CALLS = 1 << 0;
constexpr int DEBUG_PROFILER_PRINT_DIFF_THREAD_CALLS = 1 << 1;

constexpr int MAX_CHANNEL = 5;

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define CLAMP(x, low, high) (MIN((MAX((x), (low))), (high)))
#define BOOLEAN(v) (v ? "true" : "false")

#define AssertNullptr(_p) \
    if (_p == nullptr)    \
    Error(#_p " is nullptr in %s", __FUNCTION__)

#define AssertMatched(_p1, _p2) \
    if (_p1 != _p2)             \
    Error(#_p1 " mismatch with " #_p2 " in %s", __FUNCTION__)

TransmitManager g_Transmit;
SMEXT_LINK(&g_Transmit);

using read_guard   = std::shared_lock<std::shared_mutex>;
using write_guard  = std::unique_lock<std::shared_mutex>;
using thread_guard = std::lock_guard<std::mutex>;

IGameConfig*         g_pGameConf = nullptr;
ISDKHooks*           g_pSDKHooks = nullptr;
IServerGameEnts*     g_pGameEnt  = nullptr;
CGlobalVars*         g_pGlobals  = nullptr;
std::shared_mutex    g_MutexHooks;
std::mutex           g_MutexTread;
int32_t              g_u64EngineThreadId;
std::atomic<int32_t> g_Counter(0);
std::atomic<int32_t> g_FrameCalls(0);
ConVar               sm_transmit_debug_profiler("sm_transmit_debug_profiler", "0", FCVAR_RELEASE, "Enable debug profiler.");

#ifdef DEBUG

#    define RLOCK                                                                     \
        Msg("ReadLocking In L%d -> thread = %llu\n", __LINE__, GetCurrentThreadId()); \
        read_guard lock(g_MutexHooks);                                                \
        Msg("ReadLock In L%d -> thread = %llu\n", __LINE__, GetCurrentThreadId())

#    define WLOCK                                                                      \
        Msg("ManuaLocking In L%d -> thread = %llu\n", __LINE__, GetCurrentThreadId()); \
        write_guard lock(g_MutexHooks);                                                \
        Msg("ManuaLocked In L%d -> thread = %llu\n", __LINE__, GetCurrentThreadId())

#else

#    define RLOCK \
        read_guard lock(g_MutexHooks)

#    define WLOCK \
        write_guard lock(g_MutexHooks)

#endif

#define TLOCK \
    thread_guard tlock(g_MutexTread)

class CHook
{
public:
    CHook(CBaseEntity* pEntity, bool defaultTransmit) :
        m_pEntity(pEntity),
        m_iEntityIndex(gamehelpers->EntityToBCompatRef(pEntity)),
        m_nOwnerEntity(-1),
        m_bDefaultTransmit(defaultTransmit),
        m_bBlockAll(false),
        m_pszClassname(V_strdup(gamehelpers->GetEntityClassname(pEntity)))
    {
        // for css, sourceTV is 65
        for (auto i = 0; i <= SM_MAXPLAYERS; i++)
        {
            // can see by default
            SetAllChannel(i, defaultTransmit);
        }

        m_bRemoveFlags = (m_pszClassname != nullptr && (V_strcasecmp(m_pszClassname, "info_particle_system") == 0 || V_strcasecmp(m_pszClassname, "light_dynamic") == 0 || V_strcasecmp(m_pszClassname, "env_cascade_light") == 0 || V_strcasecmp(m_pszClassname, "env_projectedtexture") == 0 || V_strcasecmp(m_pszClassname, "env_screenoverlay") == 0 || V_strcasecmp(m_pszClassname, "env_fog_controller") == 0 || V_strcasecmp(m_pszClassname, "env_lightglow") == 0 || V_strcasecmp(m_pszClassname, "env_particlesmokegrenade") == 0 || V_strcasecmp(m_pszClassname, "env_global_light") == 0 || V_strcasecmp(m_pszClassname, "env_sun") == 0 || V_strcasecmp(m_pszClassname, "env_sprite") == 0 || V_strcasecmp(m_pszClassname, "point_camera") == 0 || V_strcasecmp(m_pszClassname, "point_viewproxy") == 0 || V_strcasecmp(m_pszClassname, "inferno") == 0 || V_strcasecmp(m_pszClassname, "sunshine_shadow_control") == 0 || V_strcasecmp(m_pszClassname, "cfe_player_decal") == 0 || V_strcasecmp(m_pszClassname, "func_precipitation") == 0 || V_strcasecmp(m_pszClassname, "cs_ragdoll") == 0 || V_strcasecmp(m_pszClassname, "info_target") == 0 || V_strncasecmp(m_pszClassname, "point_viewcontrol", 17) == 0 || V_strncasecmp(m_pszClassname, "env_fire", 8) == 0 || V_strncasecmp(m_pszClassname, "color_correction", 16) == 0));

        CheckFlags(pEntity);

#ifdef TRACE
        Msg("Construct::%d.%s::(%s) -> m_bRemoveFlags = %s\n", m_iEntityIndex, m_pszClassname, BOOLEAN(defaultTransmit), BOOLEAN(m_bRemoveFlags));
#endif
    }

    ~CHook()
    {
        delete[] m_pszClassname;
    }

private:
    void SetAllChannel(int client, bool v)
    {
#ifdef TRACE
        if (m_iEntityIndex < SM_MAXPLAYERS)
            Msg("SetAllChannel::%d.%s::(%d, %s)\n", m_iEntityIndex, m_pszClassname, client, BOOLEAN(v));
#endif

        for (auto c = MAX_CHANNEL; c >= 0; c--)
        {
            m_bCanTransmit[client][c] = v;
        }
    }

public:
    [[nodiscard]] bool CanSee(int client) const
    {
        if (m_iEntityIndex == client)
            return true;

        for (auto c = MAX_CHANNEL; c >= 0; c--)
        {
            if (!m_bCanTransmit[client][c])
            {
#ifdef TRACE
                if (m_iEntityIndex < SM_MAXPLAYERS && playerhelpers->GetGamePlayer(m_iEntityIndex) && playerhelpers->GetGamePlayer(client))
                    Msg("CanSee[%s] -> [%s] -> channel = %d\n", playerhelpers->GetGamePlayer(m_iEntityIndex)->GetName(), playerhelpers->GetGamePlayer(client)->GetName(), c);
#endif
                return false;
            }
        }

        return true;
    }

    void SetSee(int client, bool can, int channel)
    {
#ifdef TRACE
        if (m_iEntityIndex < SM_MAXPLAYERS)
            Msg("SetSee::%d.%s::(%d, %s, %d)\n", m_iEntityIndex, m_pszClassname, client, BOOLEAN(can), channel);
#endif
        if (channel == -1)
        {
            SetAllChannel(client, can);
            return;
        }

        const auto c              = CLAMP(channel, 0, MAX_CHANNEL);
        m_bCanTransmit[client][c] = can;
    }

    [[nodiscard]] bool GetState(int client, int channel) const
    {
        return m_bCanTransmit[client][channel];
    }

    void SetDefault(int client)
    {
#ifdef TRACE
        if (m_iEntityIndex < SM_MAXPLAYERS)
            Msg("SetDefault::%d.%s::(%d)\n", client);
#endif
        SetAllChannel(client, m_bDefaultTransmit);
    }

    [[nodiscard]] int GetOwner() const
    {
        return m_nOwnerEntity;
    }

    void SetOwner(int owner)
    {
#ifdef TRACE
        if (m_iEntityIndex < SM_MAXPLAYERS)
            Msg("SetOwner::%d.%s::(%d)\n", m_iEntityIndex, m_pszClassname, owner);
#endif
        m_nOwnerEntity = owner;
    }

    void CheckFlags(const CBaseEntity* pSource) const
    {
        AssertMatched(m_pEntity, pSource);

        const auto pEdict = g_pGameEnt->BaseEntityToEdict(m_pEntity);
        // const auto pEdict = gamehelpers->EdictOfIndex(m_iEntityIndex);
        if (!pEdict || pEdict->IsFree())
        {
            smutils->LogError(myself, "Why Hooked Entity / edict is nullptr <%d.%s>", m_iEntityIndex, m_pszClassname);
            return;
        }

        const auto flags = pEdict->m_fStateFlags;
        if (flags & FL_EDICT_ALWAYS)
        {
#ifdef TRACE
            Msg("%d.%s should remove flags %d\n", m_iEntityIndex, m_pszClassname, flags);
#endif
            pEdict->m_fStateFlags = (flags ^ FL_EDICT_ALWAYS);
        }
    }

    void SetBlockAll(bool state)
    {
#ifdef TRACE
        if (m_iEntityIndex < SM_MAXPLAYERS)
            Msg("SetBlockAll::%d.%s::(%s)\n", m_iEntityIndex, m_pszClassname, BOOLEAN(state));
#endif
        m_bBlockAll = state;
    }

    [[nodiscard]] bool GetBlockAll() const
    {
        return m_bBlockAll;
    }

private:
    CBaseEntity* m_pEntity = nullptr;
    int          m_iEntityIndex;
    bool         m_bCanTransmit[SM_MAXPLAYERS + 1][MAX_CHANNEL + 1];
    int          m_nOwnerEntity;
    bool         m_bRemoveFlags;
    bool         m_bDefaultTransmit;
    bool         m_bBlockAll;
    const char*  m_pszClassname = nullptr;
};

CHook* g_pHooks[MAX_EDICTS];
int32  g_nHooks[MAX_EDICTS];

#ifdef PLATFORM_WINDOWS
void(__stdcall* DETOUR_CheckTransmit_Actual)(CCheckTransmitInfo* pInfo, const unsigned short* pEdictIndices, int nEdicts) = nullptr;
void __stdcall DETOUR_CheckTransmit(CCheckTransmitInfo* pInfo, const unsigned short* pEdictIndices, int nEdicts)
#else
DETOUR_DECL_MEMBER3(DETOUR_CheckTransmit, void, CCheckTransmitInfo*, pInfo, const unsigned short*, pEdictIndices, int, nEdicts)
#endif
{
#ifdef PLATFORM_WINDOWS
#    define CALL_CHECK \
        DETOUR_CheckTransmit_Actual(pInfo, pEdictIndices, nEdicts)
#else
#    define CALL_CHECK                           \
        DETOUR_MEMBER_CALL(DETOUR_CheckTransmit) \
        (pInfo, pEdictIndices, nEdicts)

#endif

    // NOTE why?
    // �Ҳ�֪��ΪʲôParallel�ǰ�pClient������䵽ThreadPool����,
    // ��������ThreadPool�������е�Client,
    // �⵼��ͬһ��pSnapshot����,
    // �������A�ߵ���thread 1, �����B�ߵ�thread 2
    // ������ڲ�����Դ��������,
    // ���ʹ��SOURCEHOOK�ᵼ��GlobalPtr����
    // ����Ҫ����.
    // ����ʹ��DETOUR��ʽ!

    const int32_t threadId = ThreadGetCurrentId();
    g_Counter              = 0;

    AssertNullptr(pInfo->m_pClientEnt);

    TLOCK;
    RLOCK;

    CALL_CHECK;

    if (threadId == g_u64EngineThreadId)
    {
        ++g_FrameCalls;
    }
    else if (sm_transmit_debug_profiler.GetInt() & DEBUG_PROFILER_PRINT_DIFF_THREAD_CALLS)
    {
        Msg("CheckTransmit(%02d) -> call (%04d) in mainThread = %08d | currentThread = %08d\n",
            playerhelpers->GetGamePlayer(pInfo->m_pClientEnt)->GetIndex(),
            g_Counter.load(),
            g_u64EngineThreadId,
            threadId);
    }

#ifdef POST_CHECK_HOOK
    const auto client = gamehelpers->IndexOfEdict(pInfo->m_pClientEnt);

    edict_t* pBaseEdict = g_pGlobals->pEdicts;

    for (auto i = 0; i < nEdicts; i++)
    {
        const auto index  = pEdictIndices[i];
        const auto pEdict = &pBaseEdict[index];

        if (pEdict->m_fStateFlags & 1 << 4) // FL_EDICT_DONTSEND
        {
            continue;
        }

        if (!pInfo->m_pTransmitEdict->Get(index))
        {
            continue;
        }

        const auto pEntity = g_pGameEnt->EdictToBaseEntity(pEdict);
        const auto entity  = gamehelpers->EntityToBCompatRef(pEntity);

        if (!IsEntityIndexInRange(entity))
        {
            continue;
        }

        if (g_pHooks[entity] == nullptr)
        {
            continue;
        }

        if (!g_pHooks[entity]->CanSee(client))
        {
            if (pInfo->m_pTransmitEdict->Get(index))
                pInfo->m_pTransmitEdict->Clear(index);
            continue;
        }

        const auto owner = g_pHooks[entity]->GetOwner();
        if (client == owner)
        {
            continue;
        }

        if (g_pHooks[entity]->GetBlockAll())
        {
            if (pInfo->m_pTransmitEdict->Get(index))
                pInfo->m_pTransmitEdict->Clear(index);
            continue;
        }

        if (owner != -1 && g_pHooks[owner] != nullptr && !g_pHooks[owner]->CanSee(client))
        {
            if (pInfo->m_pTransmitEdict->Get(index))
                pInfo->m_pTransmitEdict->Clear(index);
            continue;
        }

        // passing
    }
#endif
}

#ifdef DETOUR_TRANSMIT
DECL_TRANSMIT_DETOUR(1)
DECL_TRANSMIT_DETOUR(2)
DECL_TRANSMIT_DETOUR(3)
DECL_TRANSMIT_DETOUR(4)
DECL_TRANSMIT_DETOUR(5)
#endif

SH_DECL_MANUALHOOK2_void(SetTransmit, 0, 0, 0, CCheckTransmitInfo*, bool);

void Hook_SetTransmit(CCheckTransmitInfo* pInfo, bool bAlways)
{
    ++g_Counter;

    // probably double lock in sync mode
    RLOCK;

    const auto pEntity = META_IFACEPTR(CBaseEntity);
    const auto entity  = gamehelpers->EntityToBCompatRef(pEntity);

    if (!IsEntityIndexInRange(entity) || g_pHooks[entity] == nullptr)
    {
        RETURN_META(MRES_IGNORED);
    }

    g_pHooks[entity]->CheckFlags(pEntity);

    const auto client = gamehelpers->IndexOfEdict(pInfo->m_pClientEnt);
    if (client == -1)
    {
        RETURN_META(MRES_IGNORED);
    }

    if (!g_pHooks[entity]->CanSee(client))
    {
        RETURN_META(MRES_SUPERCEDE);
    }

    const auto owner = g_pHooks[entity]->GetOwner();
    if (owner == client)
    {
        RETURN_META(MRES_IGNORED);
    }

    if (g_pHooks[entity]->GetBlockAll())
    {
        RETURN_META(MRES_SUPERCEDE);
    }

    if (owner != -1 && g_pHooks[owner] != nullptr && !g_pHooks[owner]->CanSee(client))
    {
        RETURN_META(MRES_SUPERCEDE);
    }

    RETURN_META(MRES_IGNORED);
}

void OnGameFrame(bool simulating)
{
    if (g_FrameCalls.load() > 0 && sm_transmit_debug_profiler.GetInt() & DEBUG_PROFILER_PRINT_MAIN_THREAD_CALLS && g_FrameCalls.load() > playerhelpers->GetNumPlayers() / 2)
        Msg("Called in main thread: %d times with %d players\n", g_FrameCalls.load(), playerhelpers->GetNumPlayers());

    g_FrameCalls = 0;

    g_Transmit.CheckParallel();
}

bool TransmitManager::SDK_OnMetamodLoad(ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetServerFactory, g_pGameEnt, IServerGameEnts, INTERFACEVERSION_SERVERGAMEENTS);

    g_pGlobals = ismm->GetCGlobals();
    ConVar_Register(0, this);

    return true;
}

bool TransmitManager::RegisterConCommandBase(ConCommandBase* pVar)
{
    return META_REGCVAR(pVar);
}

bool TransmitManager::SDK_OnLoad(char* error, size_t maxlength, bool late)
{
    g_u64EngineThreadId = ThreadGetCurrentId();

    sharesys->AddDependency(myself, "sdkhooks.ext", true, true);
    SM_GET_IFACE(SDKHOOKS, g_pSDKHooks);

    if (!gameconfs->LoadGameConfigFile("transmit.games", &g_pGameConf, error, maxlength))
    {
        smutils->Format(error, maxlength, "Failed to load Transmit GameData.");
        return false;
    }

    IGameConfig* conf;
    if (!gameconfs->LoadGameConfigFile("sdkhooks.games", &conf, error, maxlength))
    {
        smutils->Format(error, maxlength, "Failed to load SDKHooks GameData.");
        return false;
    }

    auto offset = -1;
    if (!conf->GetOffset("SetTransmit", &offset))
    {
        gameconfs->CloseGameConfigFile(conf);
        smutils->Format(error, maxlength, "Failed to load 'SetTransmit' offset.");
        return false;
    }
    gameconfs->CloseGameConfigFile(conf);
    SH_MANUALHOOK_RECONFIGURE(SetTransmit, offset, 0, 0);

    CDetourManager::Init(smutils->GetScriptingEngine(), g_pGameConf);

    // CServerGameEnts::CheckTransmit
    m_pDetour[0] =
#ifdef PLATFORM_WINDOWS
        DETOUR_CREATE_STATIC(DETOUR_CheckTransmit, "CServerGameEnts::CheckTransmit");
#else
        DETOUR_CREATE_MEMBER(DETOUR_CheckTransmit, "CServerGameEnts::CheckTransmit");
#endif
    if (m_pDetour[0] == nullptr)
    {
        smutils->Format(error, maxlength, "Detour<%s> is nullptr", "CServerGameEnts::CheckTransmit");
        return false;
    }

#ifdef DETOUR_TRANSMIT

#    define CREATE_TRANSMIT_DETOUR(n)                                                                               \
        m_pDetour[n] = DETOUR_CREATE_MEMBER(DETOUR_SetTransmit##n, "SetTransmit::" #n) if (m_pDetour[n] == nullptr) \
            Error("Detour<%s> is nullptr", "SetTransmit::" #n);                                                     \
        m_pDetour[n]->EnableDetour()

    CREATE_TRANSMIT_DETOUR(1);
    CREATE_TRANSMIT_DETOUR(2);
    CREATE_TRANSMIT_DETOUR(3);
    CREATE_TRANSMIT_DETOUR(4);
    CREATE_TRANSMIT_DETOUR(5);
#endif

    memset(g_nHooks, 0, sizeof(g_nHooks));

    playerhelpers->AddClientListener(this);
    g_pSDKHooks->AddEntityListener(this);

    extern sp_nativeinfo_t g_Natives[];
    g_pShareSys->AddNatives(myself, g_Natives);
    g_pShareSys->RegisterLibrary(myself, "TransmitManager");

    sv_parallel_send = g_pCVar->FindVar("sv_parallel_send");
    if (sv_parallel_send != nullptr)
    {
        m_bParallel = sv_parallel_send->GetBool();
    }

    if (m_bParallel)
    {
        m_pDetour[0]->EnableDetour();
        smutils->LogMessage(myself, "Initialized as parallel mode <thread-%d>!", g_u64EngineThreadId);

#if PLATFORM_WINDOWS
        const auto pThread  = GetCurrentThread();
        const auto priority = GetThreadPriority(pThread);
        if (SetThreadPriority(pThread, THREAD_PRIORITY_TIME_CRITICAL))
            smutils->LogMessage(myself, "Increased main thread to %d from %d", THREAD_PRIORITY_TIME_CRITICAL, priority);
#endif
    }
    else
    {
        smutils->LogMessage(myself, "Initialized as synchronization mode!");
    }
    m_bDelayDetourDisable = false;

    smutils->AddGameFrameHook(&OnGameFrame);

    return true;
}

void TransmitManager::CheckParallel()
{
    if (sv_parallel_send == nullptr)
        return;

    TLOCK;

    const auto bParallel = sv_parallel_send->GetBool();

    if (bParallel != m_bParallel)
        smutils->LogMessage(myself, "switch to %s mode!", bParallel ? "parallel" : "synchronization");

    m_bParallel = bParallel;

    if (!m_bParallel && m_pDetour[0]->IsEnabled())
    {
        // NOTE Later disable
        // If disable function while multi-threading executing
        // server crash instantly.
        m_bDelayDetourDisable = true;
    }
    else if (m_bParallel && !m_pDetour[0]->IsEnabled())
    {
        m_pDetour[0]->EnableDetour();
    }

    if (m_bParallel)
        m_bDelayDetourDisable = false;
}

void TransmitManager::OnCoreMapEnd()
{
    g_Counter    = 0;
    g_FrameCalls = 0;
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
    gameconfs->CloseGameConfigFile(g_pGameConf);

    // I don't know why SDKHooks dropped first.
    if (g_pSDKHooks != nullptr)
        g_pSDKHooks->RemoveEntityListener(this);

    for (const auto& detour : m_pDetour)
    {
        if (detour)
            detour->Destroy();
    }

    WLOCK;

    for (const auto& hook : g_pHooks)
    {
        delete hook;
    }
    for (auto& hook : g_nHooks)
    {
        if (hook)
        {
            SH_REMOVE_HOOK_ID(hook);
        }
        hook = 0;
    }
}

void TransmitManager::OnEntityDestroyed(CBaseEntity* pEntity)
{
    const auto entity = gamehelpers->EntityToBCompatRef(pEntity);

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
    WLOCK;

    for (auto i = 1; i < MAX_EDICTS; i++)
    {
        if (g_pHooks[i] == nullptr)
        {
            // not being hook
            continue;
        }

        g_pHooks[i]->SetDefault(client);
    }
}

void TransmitManager::OnClientDisconnecting(int client)
{
    UnhookEntity(client);
}

void TransmitManager::OnClientDisconnected(int client)
{
    if (!m_bDelayDetourDisable)
        return;

    // check if no any client connected

    for (auto i = 1; i <= playerhelpers->GetMaxClients(); i++)
    {
        const auto pPlayer = playerhelpers->GetGamePlayer(i);
        if (pPlayer == nullptr || !pPlayer->IsInGame())
            continue;

        // found in-game client
        return;
    }

    if (m_pDetour[0]->IsEnabled())
        m_pDetour[0]->DisableDetour();

    m_bDelayDetourDisable = false;
}

void TransmitManager::HookEntity(CBaseEntity* pEntity, bool defaultTransmit)
{
    // NOTE lock is not needed because call from native only!!!

    auto index = gamehelpers->EntityToBCompatRef(pEntity);

    if (!IsEntityIndexInRange(index))
    {
        // out-of-range
        smutils->LogError(myself, "Failed to hook entity %d -> out-of-range.", index);
        return;
    }

    if (g_pHooks[index] != nullptr)
    {
        smutils->LogError(myself, "Entity Hook listener [%d] is not nullptr.", index);
        return;
    }

    g_pHooks[index] = new CHook(pEntity, defaultTransmit);
    g_nHooks[index] = SH_ADD_MANUALHOOK(SetTransmit, pEntity, SH_STATIC(&Hook_SetTransmit), false);

#ifdef TRACE
    Msg("Hooked Entity (%d, %s)\n", index, BOOLEAN(defaultTransmit));
#endif

#ifdef DEBUG
    smutils->LogMessage(myself, "Hooked entity %d", index);
#endif
}

void TransmitManager::UnhookEntity(int index)
{
    WLOCK;

    if (g_pHooks[index] == nullptr)
    {
        // smutils->LogError(myself, "Entity Hook listener %d is nullptr.
        // Skipped.", index);
        return;
    }

    delete g_pHooks[index];
    g_pHooks[index] = nullptr;

    if (g_nHooks[index])
    {
        SH_REMOVE_HOOK_ID(g_nHooks[index]);
    }
    g_nHooks[index] = 0;

#ifdef TRACE
    Msg("UnHooked Entity(%d)\n", index);
#endif

#ifdef DEBUG
    smutils->LogMessage(myself, "Unhooked entity %d", index);
#endif
}

static cell_t Native_SetEntityOwner(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    WLOCK;

    if (g_pHooks[params[1]] != nullptr)
    {
        g_pHooks[params[1]]->SetOwner(params[2]);
        return true;
    }

    if (params[1] >= 1 && params[1] <= playerhelpers->GetMaxClients())
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

    WLOCK;

    // 1 = entity
    // 2 = client
    // 3 = state
    // 4 = channel

    auto channel = 0;
    if (params[0] >= 4)
        channel = params[4];

    if (g_pHooks[params[1]] != nullptr)
    {
        g_pHooks[params[1]]->SetSee(params[2], !!params[3], channel);
        return true;
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

    auto defaultTransmit = true;
    if (params[0] >= 2)
        defaultTransmit = !!params[2];

    WLOCK;

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

    const auto pEntity = gamehelpers->ReferenceToEntity(index);
    if (!pEntity)
    {
        // nuull
        return pContext->ThrowNativeError("Entity %d is invalid.", index);
    }

    g_Transmit.UnhookEntity(index);

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

    RLOCK;

    if (g_pHooks[params[1]] == nullptr)
    {
        // can see
        return pContext->ThrowNativeError("Entity %d is not being hook!", params[1]);
    }

    if (params[0] < 3)
        return g_pHooks[params[1]]->CanSee(params[2]);

    const auto channel = params[3];
    if (channel == -1)
        return g_pHooks[params[1]]->CanSee(params[2]);

    return g_pHooks[params[1]]->GetState(params[2], CLAMP(channel, 0, MAX_CHANNEL));
}

static cell_t Native_GetEntityBlock(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    RLOCK;

    if (g_pHooks[params[1]] == nullptr)
    {
        // can see
        return false;
    }

    return g_pHooks[params[1]]->GetBlockAll();
}

static cell_t Native_SetEntityBlock(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    WLOCK;

    if (g_pHooks[params[1]] == nullptr)
    {
        // can see
        return false;
    }

    g_pHooks[params[1]]->SetBlockAll(!!params[2]);
    return true;
}

static cell_t Native_IsEntityHooked(IPluginContext* pContext, const cell_t* params)
{
    if (!IsEntityIndexInRange(params[1]))
    {
        // out-of-range
        return pContext->ThrowNativeError("Entity %d is out-of-range.", params[1]);
    }

    RLOCK;

    return g_pHooks[params[1]] != nullptr;
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
