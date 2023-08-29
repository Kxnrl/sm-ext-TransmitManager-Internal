#ifndef _INCLUDE_MACRO_PROPER_H_
#define _INCLUDE_MACRO_PROPER_H_

#define DECL_TRANSMIT_DETOUR(p)                                                                                                               \
    DETOUR_DECL_MEMBER2(DETOUR_SetTransmit##p, void, CCheckTransmitInfo*, pInfo, bool, bAlways)                                               \
    {                                                                                                                                         \
        AssertNullptr(this);                                                                                                                  \
        AssertNullptr(pInfo);                                                                                                                 \
        g_Counter++;                                                                                                                          \
                                                                                                                                              \
        auto       pEntity = reinterpret_cast<CBaseEntity*>(this);                                                                            \
        const auto entity  = gamehelpers->EntityToBCompatRef(pEntity);                                                                        \
                                                                                                                                              \
        if (!IsEntityIndexInRange(entity))                                                                                                    \
        {                                                                                                                                     \
            /*smutils->LogError(myself, "Invalid CBaseEntity<%d.%s> SetTransmit CALL!!", entity, gamehelpers->GetEntityClassname(pEntity));*/ \
            /*DETOUR_MEMBER_CALL(DETOUR_SetTransmit##p)*/                                                                                     \
            /*(pInfo, bAlways);*/                                                                                                             \
            return;                                                                                                                           \
        }                                                                                                                                     \
                                                                                                                                              \
        /*if (p > 1) */                                                                                                                       \
        /*    Msg("DETOUR_SetTransmit%d -> %d.%s\n", p, entity, gamehelpers->GetEntityClassname(pEntity)); */                                 \
                                                                                                                                              \
        /*NOTE if RootDetour is enabled, per entity read lock is not needed*/                                                                 \
        if (g_pHooks[entity] == nullptr)                                                                                                      \
        {                                                                                                                                     \
            DETOUR_MEMBER_CALL(DETOUR_SetTransmit##p)                                                                                         \
            (pInfo, bAlways);                                                                                                                 \
            return;                                                                                                                           \
        }                                                                                                                                     \
                                                                                                                                              \
        g_pHooks[entity]->CheckFlags(pEntity);                                                                                                \
                                                                                                                                              \
        const auto client = gamehelpers->IndexOfEdict(pInfo->m_pClientEnt);                                                                   \
        if (client == -1)                                                                                                                     \
        {                                                                                                                                     \
            DETOUR_MEMBER_CALL(DETOUR_SetTransmit##p)                                                                                         \
            (pInfo, bAlways);                                                                                                                 \
            return;                                                                                                                           \
        }                                                                                                                                     \
                                                                                                                                              \
        /* RLOCK; */                                                                                                                          \
        /* if we need block it with owner stats = transmit */                                                                                 \
                                                                                                                                              \
        if (!g_pHooks[entity]->CanSee(client))                                                                                                \
        {                                                                                                                                     \
            /* blocked */                                                                                                                     \
            return;                                                                                                                           \
        }                                                                                                                                     \
                                                                                                                                              \
        const auto owner = g_pHooks[entity] -> GetOwner();                                                                                      \
        if (owner == client)                                                                                                                  \
        {                                                                                                                                     \
            /* don't block children by default */                                                                                             \
            DETOUR_MEMBER_CALL(DETOUR_SetTransmit##p)                                                                                         \
            (pInfo, bAlways);                                                                                                                 \
            return;                                                                                                                           \
        }                                                                                                                                     \
                                                                                                                                              \
        /* if entity shouldn't transmit for others */                                                                                         \
        if (g_pHooks[entity]->GetBlockAll())                                                                                                  \
        {                                                                                                                                     \
            /* blocked */                                                                                                                     \
            return;                                                                                                                           \
        }                                                                                                                                     \
                                                                                                                                              \
        if (owner != -1 && g_pHooks[owner] != nullptr && !g_pHooks[owner]->CanSee(client))                                                    \
        {                                                                                                                                     \
            /* blocked */                                                                                                                     \
            return;                                                                                                                           \
        }                                                                                                                                     \
                                                                                                                                              \
        DETOUR_MEMBER_CALL(DETOUR_SetTransmit##p)                                                                                             \
        (pInfo, bAlways);                                                                                                                     \
    }

#endif // _INCLUDE_MACRO_PROPER_H_