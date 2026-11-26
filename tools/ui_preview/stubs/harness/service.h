#pragma once

#include "harness/catalog.h"
#include "harness/upload.h"
#include "application.h"
#include <functional>
#include <memory>

namespace harness {
inline const char*& PreviewConnectionLabel() {
    static const char* label = "接收（未连接）";
    return label;
}
inline bool& PreviewReady() { static bool ready = false; return ready; }
inline uint32_t& PreviewRevision() { static uint32_t revision = 0; return revision; }
inline auto& PreviewCatalog() {
    static std::function<void(const CatalogRequest&, const std::shared_ptr<CatalogCall>&)> handler;
    return handler;
}
inline const char* ConnectionStatusLabel() { return PreviewConnectionLabel(); }
inline bool ServiceReady() { return PreviewReady(); }
inline ApprovalView& PreviewApproval() { static ApprovalView view; return view; }
inline ApprovalView TrackedApproval(const std::string& id) {
    const auto& view = PreviewApproval();
    return view.info && view.info->id == id ? view : ApprovalView{};
}
inline std::vector<ApprovalPtr> PendingApprovals() {
    const auto& view = PreviewApproval();
    return view.info && view.info->state == ApprovalState::Pending && !view.checking
        ? std::vector<ApprovalPtr>{view.info} : std::vector<ApprovalPtr>{};
}
inline bool RequestApproval(const ApprovalPtr& approval, ApprovalDecision) {
    auto& view = PreviewApproval();
    if (!view.info || view.info != approval || view.submitting) return false;
    view.submitting = true;
    return true;
}
inline RunView& PreviewRun() { static RunView run; return run; }
inline RunView TrackedRun(const std::string& session) {
    return PreviewRun().run.session_id == session ? PreviewRun() : RunView{};
}
inline bool RequestRunCancel(const std::string& session, const std::string& run) {
    auto& task = PreviewRun();
    if (task.run.session_id != session || task.run.id != run || task.run.Finished()) return false;
    task.cancelling = true;
    return true;
}
inline UploadStatus& PreviewRecording() { static UploadStatus status; return status; }
inline UploadStatus RecordingStatus() { return PreviewRecording(); }
inline bool QueueRecording(std::string session, uint32_t, uint32_t) {
    if (PreviewRecording().Pending()) return false;
    PreviewRecording() = {UploadPhase::Queued, std::move(session), {}, {}};
    return true;
}
inline bool RequestRecordingAction(UploadAction action) {
    if (!PreviewRecording().HasActions()) return false;
    if (action == UploadAction::Discard) {
        Application::GetInstance().GetAudioService().ReleaseLocalRecording();
        PreviewRecording().phase = UploadPhase::Discarded;
    } else {
        PreviewRecording().phase = UploadPhase::Queued;
    }
    return true;
}
inline uint32_t CatalogRevision() { return PreviewRevision(); }
inline std::weak_ptr<HistoryInbox>& PreviewHistory() {
    static std::weak_ptr<HistoryInbox> inbox;
    return inbox;
}
inline std::shared_ptr<HistoryInbox> SubscribeHistory(const std::string& session_id) {
    auto inbox = std::make_shared<HistoryInbox>(session_id);
    PreviewHistory() = inbox;
    return inbox;
}
inline std::shared_ptr<CatalogCall> SubmitCatalog(CatalogRequest request) {
    auto call = std::make_shared<CatalogCall>();
    PreviewCatalog()(request, call);
    return call;
}
}
