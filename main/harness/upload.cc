#include "upload.h"

#include "client.h"
#include "transport.h"

#include <algorithm>
#include <cmath>
#include <cJSON.h>
#include <utility>

namespace harness {
namespace {

constexpr unsigned kMaxAttempts = 3;
constexpr uint64_t kAckTimeoutMs = 10000;
constexpr size_t kMinPcmBytes = 6400;
constexpr size_t kMaxPcmBytes = 960000;

const cJSON* Field(const cJSON* object, const char* key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

std::string String(const cJSON* object, const char* key) {
    const auto* value = Field(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

bool IsId(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) { return c > 32 && c < 127; });
}

bool SendCommand(ClientIo& io, const char* type, const std::string& command,
                 const std::string& session, const std::string& utterance,
                 const std::function<bool(cJSON*)>& add_fields) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    if (!root ||
        !cJSON_AddNumberToObject(root.get(), "v", 1) ||
        !cJSON_AddStringToObject(root.get(), "type", type) ||
        !cJSON_AddStringToObject(root.get(), "command_id", command.c_str()) ||
        !cJSON_AddStringToObject(root.get(), "session_id", session.c_str())) return false;
    auto* data = cJSON_AddObjectToObject(root.get(), "data");
    if (!data || !cJSON_AddStringToObject(data, "utterance_id", utterance.c_str()) || !add_fields(data)) {
        return false;
    }
    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(root.get()), cJSON_free);
    return body && io.SendText(body.get());
}

}  // namespace

UploadTicket::UploadTicket(std::string session, uint32_t version, std::string utterance, RecordingSource source)
    : settings_version_(version), utterance_(std::move(utterance)), source_(std::move(source)) {
    status_.phase = UploadPhase::Queued;
    status_.session_id = std::move(session);
}
UploadStatus UploadTicket::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}
bool UploadTicket::Request(UploadAction action) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!status_.HasActions() || action_ != UploadAction::None || action == UploadAction::None ||
        (action == UploadAction::Retry && !status_.Retryable())) return false;
    action_ = action;
    return true;
}

void UploadTicket::Observe(const std::string& event) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(ParseJsonMessage(event.data(), event.size()), cJSON_Delete);
    const auto type = String(root.get(), "type");
    if (type != "asr.final" && type != "run.failed") return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.phase != UploadPhase::Accepted || String(root.get(), "run_id") != status_.run_id ||
        String(root.get(), "session_id") != status_.session_id) return;
    const auto* data = Field(root.get(), "data");
    if (type == "asr.final") {
        auto id = String(data, "message_id");
        if (String(data, "utterance_id") == utterance_ && IsId(id)) status_.transcript_id = std::move(id);
    } else {
        const auto code = String(Field(data, "error"), "code");
        if (code == "asr_failed" || code == "asr_no_speech") status_.error = code;
    }
}

void UploadTransfer::Publish(UploadPhase phase, std::string error) {
    std::lock_guard<std::mutex> lock(ticket_->mutex_);
    ticket_->status_.phase = phase;
    ticket_->status_.error = std::move(error);
}

void UploadTransfer::ResetWire() {
    wire_ = Wire::Idle;
    command_.clear();
    upload_id_.clear();
    offset_ = 0;
    chunk_limit_ = 0;
}

void UploadTransfer::Fail(const std::string& error, bool automatic) {
    ResetWire();
    Publish(automatic && attempts_ < kMaxAttempts ? UploadPhase::WaitingNetwork : UploadPhase::RetryRequired, error);
}

void UploadTransfer::Finish(UploadPhase phase, std::string run) {
    ResetWire();
    // Preserve the run ID even after the PCM is freed.
    ticket_->source_.release();
    std::lock_guard<std::mutex> lock(ticket_->mutex_);
    ticket_->status_.run_id = std::move(run);
    ticket_->status_.phase = phase;
    ticket_->status_.error.clear();
    ticket_->action_ = UploadAction::None;
}

bool UploadTransfer::Sending() const { return wire_ != Wire::Idle; }

bool UploadTransfer::Step(ClientIo& io, std::shared_ptr<UploadTicket> ticket, const std::string& connection,
                          uint64_t now, const std::function<std::string()>& new_command) {
    if (ticket_ != ticket) {
        ticket_ = std::move(ticket);
        digest_.clear();
        connection_.clear();
        attempts_ = 0;
        ResetWire();
    }
    if (!ticket_) return false;
    UploadStatus status;
    UploadAction action;
    {
        std::lock_guard<std::mutex> lock(ticket_->mutex_);
        status = ticket_->status_;
        action = std::exchange(ticket_->action_, UploadAction::None);
        // Reserve the next send under the same lock as UI actions.
        if (!connection.empty() && wire_ == Wire::Idle && status.Pending() &&
            (action == UploadAction::Retry || status.phase == UploadPhase::Queued ||
             (status.phase == UploadPhase::WaitingNetwork && connection_ != connection))) {
            ticket_->status_.phase = UploadPhase::Sending;
        }
    }
    if (action == UploadAction::Discard) {
        Finish(UploadPhase::Discarded);
        return false;
    }
    if (action == UploadAction::Retry) {
        attempts_ = 0;
        connection_.clear();
        status.phase = UploadPhase::Queued;
        Publish(UploadPhase::Queued);
    }
    if (!status.Pending() || status.phase == UploadPhase::RetryRequired) return false;
    if (connection.empty()) {
        if (Sending()) Fail("connection_lost", true);
        else if (status.phase == UploadPhase::Queued) Publish(UploadPhase::WaitingNetwork, "connection_lost");
        return false;
    }
    if (wire_ != Wire::Idle && connection_ != connection) Fail("connection_lost", true);
    if (wire_ == Wire::Idle) {
        if (connection_ == connection) return false;
        if (attempts_ >= kMaxAttempts) {
            Fail("confirmation_unknown", false);
            return false;
        }
        const auto bytes = ticket_->source_.bytes;
        if (bytes < kMinPcmBytes || bytes > kMaxPcmBytes || bytes % 2 || !ticket_->settings_version_ ||
            !IsId(ticket_->utterance_) || !IsId(status.session_id)) {
            Fail("invalid_audio", false);
            return false;
        }
        Publish(UploadPhase::Sending);
        connection_ = connection;
        ++attempts_;
        if (digest_.empty()) digest_ = ticket_->source_.sha256();
        if (digest_.size() != 64) {
            Fail("recording_read_failed", false);
            return false;
        }
        command_ = new_command();
        wire_ = Wire::BeginAck;
        deadline_ = now + kAckTimeoutMs;
        if (!SendCommand(io, "audio.begin", command_, status.session_id, ticket_->utterance_, [&](cJSON* data) {
            return cJSON_AddNumberToObject(data, "expected_settings_version", ticket_->settings_version_) &&
                   cJSON_AddStringToObject(data, "format", "pcm_s16le") &&
                   cJSON_AddNumberToObject(data, "sample_rate", 16000) &&
                   cJSON_AddNumberToObject(data, "channels", 1) &&
                   cJSON_AddNumberToObject(data, "duration_ms", bytes / 32) &&
                   cJSON_AddNumberToObject(data, "total_bytes", bytes) &&
                   cJSON_AddStringToObject(data, "sha256", digest_.c_str());
        })) {
            Fail("send_failed", true);
            return true;
        }
        return false;
    }
    if (wire_ == Wire::Body) {
        uint8_t bytes[kMaxAudioChunkBytes];
        const size_t count = std::min(chunk_limit_, ticket_->source_.bytes - offset_);
        if (!ticket_->source_.read(offset_, bytes, count)) {
            Fail("recording_read_failed", false);
            return true;
        }
        if (!io.SendBinary(bytes, count)) {
            Fail("send_failed", true);
            return true;
        }
        offset_ += count;
        if (offset_ < ticket_->source_.bytes) return false;
        command_ = new_command();
        wire_ = Wire::EndAck;
        deadline_ = now + kAckTimeoutMs;
        if (!SendCommand(io, "audio.end", command_, status.session_id, ticket_->utterance_, [&](cJSON* data) {
            return cJSON_AddStringToObject(data, "upload_id", upload_id_.c_str()) != nullptr;
        })) {
            Fail("send_failed", true);
            return true;
        }
        return false;
    }
    if (now >= deadline_) {
        Fail("confirmation_unknown", true);
        return true;
    }
    return false;
}

bool UploadTransfer::Ack(const cJSON* envelope) {
    if ((wire_ != Wire::BeginAck && wire_ != Wire::EndAck) ||
        String(envelope, "command_id") != command_) return false;
    const auto* ok = Field(envelope, "ok");
    if (cJSON_IsFalse(ok)) {
        const auto error = String(Field(envelope, "error"), "code");
        Fail(error.empty() ? "upload_rejected" : error, false);
        return error == "upload_busy";
    }
    const auto* data = Field(envelope, "data");
    const auto status = String(data, "status");
    if (cJSON_IsTrue(ok) && status == "accepted") {
        const auto run = String(data, "run_id");
        const auto* version = Field(data, "settings_version");
        if (String(data, "utterance_id") == ticket_->utterance_ && IsId(run) &&
            cJSON_IsNumber(version) && version->valuedouble == ticket_->settings_version_ &&
            IsId(String(data, "model_id")) && IsId(String(data, "authorization_level_id"))) {
            Finish(UploadPhase::Accepted, run);
            return false;
        }
    }
    if (cJSON_IsTrue(ok) && status == "upload_required" && wire_ == Wire::BeginAck) {
        upload_id_ = String(data, "upload_id");
        const auto* limit = Field(data, "max_chunk_bytes");
        if (IsId(upload_id_) && cJSON_IsNumber(limit) && limit->valuedouble >= 2 &&
            limit->valuedouble <= kMaxAudioChunkBytes &&
            std::floor(limit->valuedouble) == limit->valuedouble) {
            chunk_limit_ = static_cast<size_t>(limit->valuedouble) & ~size_t{1};
            if (chunk_limit_ >= 2) {
                wire_ = Wire::Body;
                return false;
            }
        }
    }
    Fail("invalid_upload_ack", true);
    return true;
}

}  // namespace harness
