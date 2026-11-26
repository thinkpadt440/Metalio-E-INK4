#pragma once
#include "catalog.h"
#include "client.h"
#include <memory>
namespace harness {
enum class ServerOriginResult { Saved, Unchanged, Invalid, RecordingPending, StorageError };
// Initialize once on the internal-stack application task before the display starts.
void InitializeServerOrigin();
// Safe for LVGL. Reads the boot-time copy in memory.
std::string ConfiguredServerOrigin();
// Call on the application task. A successful change takes effect after reboot.
ServerOriginResult SaveServerOrigin(std::string origin);
void StartService();
void StopService();
ConnectionSnapshot ServiceStatus();
const char* ConnectionStatusLabel();
bool ServiceReady();
ApprovalView TrackedApproval(const std::string& id);
std::vector<ApprovalPtr> PendingApprovals();
bool RequestApproval(const ApprovalPtr& approval, ApprovalDecision decision);
RunView TrackedRun(const std::string& session_id);
bool RequestRunCancel(const std::string& session_id, const std::string& run_id);
bool QueueRecording(std::string session_id, uint32_t settings_version, uint32_t samples);
UploadStatus RecordingStatus();
bool RequestRecordingAction(UploadAction action);
uint32_t CatalogRevision();
std::shared_ptr<HistoryInbox> SubscribeHistory(const std::string& session_id);
std::shared_ptr<CatalogCall> SubmitCatalog(CatalogRequest request);
}  // namespace harness
