#include "hooks.h"

#include <sourcehook/sourcehook_impl.h>

SourceHook::Impl::CSourceHookImpl parallelSourceHook;

SourceHook::ISourceHook* g_ParallelSourceHook = &parallelSourceHook;
SourceHook::ISourceHook* g_OriginalSourceHook;