#ifndef _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_PROPER_H_

#include "smsdk_ext.h"

#include <extensions/ISDKHooks.h>

class TransmitManager : public SDKExtension, public ISMEntityListener, public IClientListener
{
public:
    bool SDK_OnLoad(char* error, size_t maxlength, bool late) override;
    void SDK_OnUnload() override;
    bool SDK_OnMetamodLoad(ISmmAPI* ismm, char* error, size_t maxlength, bool late) override;
    bool QueryRunning(char* error, size_t maxlength) override;
    void NotifyInterfaceDrop(SMInterface* pInterface) override;

    // entity listener
    void OnEntityDestroyed(CBaseEntity* pEntity) override;

    // player listener
    void OnClientPutInServer(int client) override;
    void OnClientDisconnecting(int client) override;

    // method
    void Hook_SetTransmit(CCheckTransmitInfo* pInfo, bool bAlways);
    void HookEntity(CBaseEntity* pEntity, bool defaultTransmit);
    void ResetHooks();
    void CheckHooks();

private:
    void UnhookEntity(int index);

    cell_t  m_nOffsetVTable   = 0;
    ConVar* sv_parallel_send  = nullptr;
    bool    m_bLastFrameState = false;
};

inline bool IsEntityIndexInRange(int i)
{
    return i >= 1 && i < MAX_EDICTS;
}

#endif
