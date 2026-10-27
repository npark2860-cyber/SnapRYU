#pragma once
#include "Identity.h"
#include "CemuPad/control/PairingAuthority.h"
#include <filesystem>
#include <optional>

namespace CemuPad::Store {
enum class StoreError { None,Busy,Missing,AlreadyExists,Invalid,Io,Conflict,DifferentPeer,UnsupportedPath };
enum class StoreStage { TempWritten,TempFlushed,BeforePublish,Published,TargetFlushed,BeforeReadback };
struct StoreResult {
    Control::CommitOutcome outcome=Control::CommitOutcome::NotCommitted;
    StoreError error=StoreError::None;
    bool Committed() const {return outcome==Control::CommitOutcome::Committed;}
};
struct Snapshot {
    Identity identity;
    std::uint64_t revision=0;
    std::optional<Control::TrustRecord> peer;
};
struct LoadResult {StoreError error=StoreError::None;std::optional<Snapshot> snapshot;};
// Caller supplies a dedicated CemuPad user-data directory. Never resolves APPDATA itself.
// All cooperating readers/writers use writer.lock, never rename or delete it.
// Fault hook is for deterministic failures/process crashes; default is empty.
class WindowsStore {
public:
    using FaultHook=std::function<void(StoreStage)>;
    explicit WindowsStore(std::filesystem::path root,FaultHook fault={});
    StoreResult Initialize(); // Explicit first-run action; refuses any existing state.
    LoadResult Load() const; // Never creates or repairs identity/trust.
    StoreResult Commit(const Control::TrustRecord& record,std::uint64_t expectedRevision);
    StoreResult ResetTrust(std::uint64_t expectedRevision); // Caller must stop all product sessions first.
private:
    std::filesystem::path root;
    FaultHook fault;
};
}
