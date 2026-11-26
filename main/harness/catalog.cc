#include "catalog.h"

#include "client.h"
#include "transport.h"

#include <cJSON.h>
#include <cmath>
#include <memory>
#include <utility>

namespace harness {
namespace {

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr double kMaxExactInteger = 9007199254740991.0;

const cJSON* Field(const cJSON* object, const char* name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

bool ReadString(const cJSON* node, std::string& result, bool nullable = false) {
    if (nullable && cJSON_IsNull(node)) {
        result.clear();
        return true;
    }
    if (!cJSON_IsString(node) || !node->valuestring || !node->valuestring[0]) return false;
    result = node->valuestring;
    return true;
}

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 128 || id == "." || id == "..") return false;
    for (unsigned char ch : id) {
        if (ch <= 32 || ch >= 127) return false;
    }
    return true;
}

bool ReadId(const cJSON* item, const char* key, std::string& value) {
    return ReadString(Field(item, key), value) && ValidId(value);
}

bool ReadCount(const cJSON* node, std::optional<uint64_t>& value) {
    if (cJSON_IsNull(node)) return true;
    if (!cJSON_IsNumber(node)) return false;
    const double number = node->valuedouble;
    if (number < 0 || number > kMaxExactInteger || std::floor(number) != number) return false;
    value = static_cast<uint64_t>(number);
    return true;
}

bool ReadOverview(const cJSON* item, const std::string& range, Overview& overview) {
    std::string response_range;
    if (!ReadString(Field(item, "range"), response_range) || response_range != range ||
        !ReadCount(Field(item, "sessions"), overview.sessions) ||
        !ReadCount(Field(item, "messages"), overview.messages) ||
        !ReadCount(Field(item, "total_tokens"), overview.total_tokens) ||
        !ReadCount(Field(item, "active_days"), overview.active_days) ||
        !ReadString(Field(item, "peak_hour"), overview.peak_hour, true)) return false;
    const auto* model = Field(item, "favorite_model");
    return cJSON_IsNull(model) || ReadString(Field(model, "name"), overview.favorite_model);
}

bool ReadSession(const cJSON* item, Session& session) {
    std::optional<uint64_t> version;
    if (!ReadCount(Field(item, "settings_version"), version) ||
        !version || !*version || *version > UINT32_MAX) return false;
    session.settings_version = static_cast<uint32_t>(*version);
    return ReadId(item, "id", session.id) &&
        ReadString(Field(item, "title"), session.title) &&
        ReadId(item, "model_id", session.model_id) &&
        ReadString(Field(item, "model_name"), session.model_name) &&
        ReadId(item, "authorization_level_id", session.authorization_id) &&
        ReadString(Field(item, "authorization_level_name"), session.authorization_name) &&
        ReadString(Field(item, "updated_at"), session.updated_at) &&
        ReadString(Field(item, "active_run_id"), session.active_run_id, true);
}

bool ReadOption(const cJSON* item, DirectoryOption& option) {
    const auto* availability = Field(item, "available");
    option.available = cJSON_IsTrue(availability);
    return cJSON_IsBool(availability) &&
        ReadId(item, "id", option.id) && ReadString(Field(item, "name"), option.name);
}

template <typename Item>
bool ReadPage(const cJSON* root, const std::string& cursor, int limit, CatalogPage<Item>& result,
              bool (*read)(const cJSON*, Item&)) {
    const auto* items = Field(root, "items");
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) > limit ||
        !ReadString(Field(root, "next_cursor"), result.next_cursor, true) ||
        result.next_cursor.size() > 1024 ||
        (!cursor.empty() && result.next_cursor == cursor)) return false;
    const cJSON* item;
    cJSON_ArrayForEach(item, items) {
        Item value;
        if (!read(item, value)) return false;
        result.items.push_back(std::move(value));
    }
    return true;
}

std::string PageQuery(int limit, const std::string& cursor) {
    auto query = "?limit=" + std::to_string(limit);
    if (!cursor.empty()) query += "&cursor=" + UrlEncode(cursor);
    return query;
}

std::string SettingsBody(const CatalogRequest& request, bool model) {
    if (!ValidId(request.value) || request.settings_version == 0) return {};
    Json json(cJSON_CreateObject(), cJSON_Delete);
    if (!json ||
        !cJSON_AddNumberToObject(json.get(), "expected_settings_version", request.settings_version) ||
        !cJSON_AddStringToObject(json.get(), model ? "model_id" : "authorization_level_id", request.value.c_str())) {
        return {};
    }
    char* text = cJSON_PrintUnformatted(json.get());
    if (!text) return {};
    std::string result(text);
    cJSON_free(text);
    return result;
}

}  // namespace

CatalogReply FetchCatalog(ClientIo& io, const std::string& device_id, const CatalogRequest& request) {
    using Op = CatalogOperation;
    const auto op = request.operation;
    std::string method = "GET";
    std::string path = "/api/v1";
    std::string body;
    switch (op) {
        case Op::Overview:
            if (request.key != "all" && request.key != "30d" && request.key != "7d")
                return {CatalogError::RequestFailed, {}};
            path += "/dashboard/overview?range=" + request.key;
            break;
        case Op::Approval:
            if (!ValidId(request.key)) return {CatalogError::RequestFailed, {}};
            path += "/approvals/" + UrlEncode(request.key);
            break;
        case Op::Run:
            if (!ValidId(request.key)) return {CatalogError::RequestFailed, {}};
            path += "/runs/" + UrlEncode(request.key);
            break;
        case Op::History:
            if (!ValidId(request.key)) return {CatalogError::RequestFailed, {}};
            path += "/sessions/" + UrlEncode(request.key) + "/messages?limit=" + std::to_string(kHistoryLimit);
            if (!request.cursor.empty()) path += "&before=" + UrlEncode(request.cursor);
            break;
        case Op::Sessions:
            path += "/sessions" + PageQuery(kRecentSessionLimit, {});
            break;
        case Op::Models:
        case Op::Authorizations:
            path += op == Op::Models ? "/models" : "/authorization-levels";
            path += PageQuery(kDirectoryPageSize, request.cursor);
            break;
        case Op::Casual:
            if (!ValidId(device_id)) return {CatalogError::RequestFailed, {}};
            method = "PUT";
            path += "/devices/" + UrlEncode(device_id) + "/default-casual-session";
            break;
        case Op::Session:
        case Op::SetModel:
        case Op::SetAuthorization:
            if (!ValidId(request.key)) return {CatalogError::RequestFailed, {}};
            path += "/sessions/" + UrlEncode(request.key);
            if (op != Op::Session) {
                method = "PATCH";
                path += "/settings";
                body = SettingsBody(request, op == Op::SetModel);
                if (body.empty()) return {CatalogError::RequestFailed, {}};
            }
            break;
    }

    auto response = io.Request(method, path, body);
    const bool conflict = method == "PATCH" && response.status == 409;
    if ((op == Op::Run || op == Op::Approval) && response.error.empty() && response.status == 404)
        return {CatalogError::NotFound, {}};
    if (op == Op::History && response.error.empty() && response.status == 409) {
        Json error(ParseJsonMessage(response.body.data(), response.body.size()), cJSON_Delete);
        std::string code;
        if (ReadString(Field(Field(error.get(), "error"), "code"), code) && code == "history_cursor_expired")
            return {CatalogError::CursorExpired, {}};
    }
    if (!response.error.empty() ||
        (response.status != 200 && !(op == Op::Casual && response.status == 201) && !conflict))
        return {CatalogError::RequestFailed, {}};
    Json json(ParseJsonMessage(response.body.data(), response.body.size()), cJSON_Delete);
    if (!json) return {CatalogError::InvalidResponse, {}};
    const auto* object = json.get();
    if (op == Op::Overview) {
        Overview overview;
        if (ReadOverview(object, request.key, overview)) return {CatalogError::None, std::move(overview)};
    } else if (op == Op::Approval) {
        if (auto approval = ReadApproval(object, response.body); approval && approval->id == request.key)
            return {CatalogError::None, std::move(approval)};
    } else if (op == Op::Run) {
        if (auto run = ReadRun(object); run && run->id == request.key)
            return {CatalogError::None, std::move(*run)};
    } else if (op == Op::History) {
        HistoryPage page;
        if (ReadHistoryPage(object, request.cursor, page)) return {CatalogError::None, std::move(page)};
    } else if (op == Op::Sessions) {
        Sessions page;
        if (ReadPage(object, {}, kRecentSessionLimit, page, ReadSession)) {
            page.next_cursor.clear();
            return {CatalogError::None, std::move(page)};
        }
    } else if (op == Op::Models || op == Op::Authorizations) {
        Directory page;
        if (ReadPage(object, request.cursor, kDirectoryPageSize, page, ReadOption))
            return {CatalogError::None, std::move(page)};
    } else {
        Session session;
        if (op == Op::Casual) object = Field(object, "session");
        if (conflict) {
            const auto* error = Field(object, "error");
            std::string code;
            if (!ReadString(Field(error, "code"), code) || code != "settings_conflict")
                return {CatalogError::RequestFailed, {}};
            object = Field(Field(error, "details"), "current_session");
        }
        if (ReadSession(object, session) && (op == Op::Casual || session.id == request.key)) {
            return {conflict ? CatalogError::Conflict : CatalogError::None, std::move(session)};
        }
    }
    return {CatalogError::InvalidResponse, {}};
}

bool IsSessionUpdate(const std::string& event) {
    Json root(ParseJsonMessage(event.data(), event.size()), cJSON_Delete);
    return cJSON_IsString(Field(root.get(), "type")) &&
        std::string(Field(root.get(), "type")->valuestring) == "session.updated";
}

}  // namespace harness
