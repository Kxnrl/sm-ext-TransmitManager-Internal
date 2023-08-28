#ifndef _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_

#include "smsdk_ext.h"

#include <extensions/ISDKHooks.h>
class CDetour;

class TransmitManager : public SDKExtension, public ISMEntityListener, public IClientListener, public IConCommandBaseAccessor
{
public:
    bool SDK_OnLoad(char* error, size_t maxlength, bool late) override;
    void SDK_OnUnload() override;
    bool SDK_OnMetamodLoad(ISmmAPI* ismm, char* error, size_t maxlength, bool late) override;
    bool RegisterConCommandBase(ConCommandBase* pVar) override;
    bool QueryRunning(char* error, size_t maxlength) override;
    void NotifyInterfaceDrop(SMInterface* pInterface) override;
    void OnCoreMapEnd() override;

    // entity listener
    void OnEntityDestroyed(CBaseEntity* pEntity) override;

    // player listener
    void OnClientPutInServer(int client) override;
    void OnClientDisconnecting(int client) override;
    void OnClientDisconnected(int client) override;

    // method
    void HookEntity(CBaseEntity* pEntity, bool defaultTransmit);
    void UnhookEntity(int index);
    void CheckParallel();

private:
    bool     m_bParallel;
    bool     m_bDelayDetourDisable;
    ConVar*  sv_parallel_send = nullptr;
    CDetour* m_pDetour[6]     = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
};

extern TransmitManager g_Transmit;

inline bool IsEntityIndexInRange(int i)
{
    return i >= 1 && i < MAX_EDICTS;
}

#endif
