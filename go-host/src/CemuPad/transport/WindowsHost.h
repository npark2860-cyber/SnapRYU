#pragma once
#include "ControlTransport.h"
#include "CemuPad/store/WindowsStore.h"

namespace CemuPad::Transport {
struct PairResult {Control::PairOutcome outcome=Control::PairOutcome::Invalid;bool responseWritten=false;};
// Explicit initialization belongs to UI/first run. These entrypoints only Load existing state.
std::unique_ptr<ControlTransport> AcceptStoredHost(Listener&,Store::WindowsStore&,Lifecycle&,const Cancellation&,Deadline);
// One provisional connection; HELLO/PAIR only. Never opens ControlSession or data authority.
PairResult PairOnce(Listener&,Store::WindowsStore&,Control::PairingAuthority&,std::uint64_t generation,
                    const Cancellation&,Deadline,const Store::Snapshot* expected=nullptr);
}
