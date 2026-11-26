#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

struct cJSON;

namespace harness {

class ClientIo;

enum class UploadPhase { Idle, Queued, Sending, WaitingNetwork, RetryRequired, Accepted, Discarded };
enum class UploadAction { None, Retry, Discard };

struct UploadStatus {
    UploadPhase phase = UploadPhase::Idle;
    std::string session_id;
    std::string run_id;
    std::string error;
    std::string transcript_id = {};

    bool Pending() const {
        return phase == UploadPhase::Queued || phase == UploadPhase::Sending ||
               phase == UploadPhase::WaitingNetwork || phase == UploadPhase::RetryRequired;
    }
    bool Retryable() const {
        return HasActions() && error != "settings_conflict" && error != "utterance_conflict" &&
               error != "invalid_audio" && error != "recording_read_failed";
    }
    bool HasActions() const {
        return phase == UploadPhase::WaitingNetwork || phase == UploadPhase::RetryRequired;
    }
};

struct RecordingSource {
    size_t bytes = 0;
    std::function<bool(size_t, void*, size_t)> read;
    std::function<std::string()> sha256;
    std::function<void()> release;
};

class UploadTicket {
public:
    UploadTicket(std::string session, uint32_t version, std::string utterance, RecordingSource source);
    UploadStatus Status() const;
    bool Request(UploadAction action);
    void Observe(const std::string& event);

private:
    friend class UploadTransfer;
    const uint32_t settings_version_;
    const std::string utterance_;
    const RecordingSource source_;
    mutable std::mutex mutex_;
    UploadStatus status_;
    UploadAction action_ = UploadAction::None;
};

class UploadTransfer {
public:
    // Runs only in the client's network task. Return true when the socket must
    // close to retire a potentially live upload_id before another attempt.
    bool Step(ClientIo& io, std::shared_ptr<UploadTicket> ticket, const std::string& connection,
              uint64_t now, const std::function<std::string()>& new_command);
    bool Ack(const cJSON* envelope);
    bool Sending() const;

private:
    enum class Wire { Idle, BeginAck, Body, EndAck };
    void Fail(const std::string& error, bool automatic);
    void Finish(UploadPhase phase, std::string run = {});
    void ResetWire();
    void Publish(UploadPhase phase, std::string error = {});

    std::shared_ptr<UploadTicket> ticket_;
    std::string connection_;
    std::string command_;
    std::string upload_id_;
    std::string digest_;
    uint64_t deadline_ = 0;
    size_t offset_ = 0;
    size_t chunk_limit_ = 0;
    unsigned attempts_ = 0;
    Wire wire_ = Wire::Idle;
};

}  // namespace harness
