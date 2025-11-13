#include "device_registry.h"

#include "settings.h"

#include <algorithm>
#include <cctype>
#include <esp_log.h>
#include <cJSON.h>

namespace {
constexpr const char* kTag = "DeviceRegistry";
constexpr const char* kNamespace = "devices";
constexpr const char* kProfilesKey = "profiles";
constexpr const char* kPreferredSessionKey = "preferred_session";

std::string NormalizeMacAddress(const std::string& mac_address) {
    std::string normalized;
    normalized.reserve(mac_address.size());
    for (char ch : mac_address) {
        if (ch == ':' || ch == '-') {
            continue;
        }
        normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return normalized;
}

DeviceProfile NormalizeProfile(const DeviceProfile& profile) {
    DeviceProfile normalized = profile;
    normalized.mac_address = NormalizeMacAddress(profile.mac_address);
    return normalized;
}

cJSON* SerializeProfile(const DeviceProfile& profile) {
    cJSON* node = cJSON_CreateObject();
    cJSON_AddStringToObject(node, "device_id", profile.device_id.c_str());
    cJSON_AddStringToObject(node, "mac", profile.mac_address.c_str());
    cJSON_AddStringToObject(node, "label", profile.label.c_str());
    cJSON_AddStringToObject(node, "description", profile.description.c_str());
    cJSON_AddStringToObject(node, "transport_hint", profile.transport_hint.c_str());
    cJSON_AddBoolToObject(node, "allow_audio", profile.allow_audio);
    cJSON_AddBoolToObject(node, "allow_notifications", profile.allow_notifications);
    cJSON_AddBoolToObject(node, "is_primary", profile.is_primary);
    return node;
}

DeviceProfile ParseProfile(const cJSON* node) {
    DeviceProfile profile;
    if (const cJSON* device_id = cJSON_GetObjectItem(node, "device_id"); cJSON_IsString(device_id)) {
        profile.device_id = device_id->valuestring;
    }
    if (const cJSON* mac = cJSON_GetObjectItem(node, "mac"); cJSON_IsString(mac)) {
        profile.mac_address = mac->valuestring;
    }
    if (const cJSON* label = cJSON_GetObjectItem(node, "label"); cJSON_IsString(label)) {
        profile.label = label->valuestring;
    }
    if (const cJSON* description = cJSON_GetObjectItem(node, "description"); cJSON_IsString(description)) {
        profile.description = description->valuestring;
    }
    if (const cJSON* transport = cJSON_GetObjectItem(node, "transport_hint"); cJSON_IsString(transport)) {
        profile.transport_hint = transport->valuestring;
    }
    if (const cJSON* allow_audio = cJSON_GetObjectItem(node, "allow_audio"); cJSON_IsBool(allow_audio)) {
        profile.allow_audio = cJSON_IsTrue(allow_audio);
    }
    if (const cJSON* allow_notifications = cJSON_GetObjectItem(node, "allow_notifications"); cJSON_IsBool(allow_notifications)) {
        profile.allow_notifications = cJSON_IsTrue(allow_notifications);
    }
    if (const cJSON* is_primary = cJSON_GetObjectItem(node, "is_primary"); cJSON_IsBool(is_primary)) {
        profile.is_primary = cJSON_IsTrue(is_primary);
    }
    return NormalizeProfile(profile);
}

void LoadPreferredSession(std::string& preferred_session_id) {
    Settings settings(kNamespace, false);
    preferred_session_id = settings.GetString(kPreferredSessionKey, "");
}

} // namespace

DeviceRegistry& DeviceRegistry::GetInstance() {
    static DeviceRegistry instance;
    return instance;
}

DeviceRegistry::DeviceRegistry() {
    LoadProfilesLocked();
    LoadPreferredSession(preferred_session_id_);
}

std::vector<DeviceProfile> DeviceRegistry::GetProfiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return profiles_;
}

bool DeviceRegistry::AddOrUpdateProfile(const DeviceProfile& profile) {
    DeviceProfile normalized = NormalizeProfile(profile);
    std::lock_guard<std::mutex> lock(mutex_);
    auto predicate = [&normalized](const DeviceProfile& candidate) {
        if (!normalized.mac_address.empty() && !candidate.mac_address.empty()) {
            return normalized.mac_address == candidate.mac_address;
        }
        if (!normalized.device_id.empty() && !candidate.device_id.empty()) {
            return normalized.device_id == candidate.device_id;
        }
        return false;
    };
    auto it = std::find_if(profiles_.begin(), profiles_.end(), predicate);
    if (it != profiles_.end()) {
        *it = normalized;
    } else {
        profiles_.push_back(normalized);
    }
    PersistProfilesLocked();
    return true;
}

bool DeviceRegistry::RemoveProfileByMac(const std::string& mac_address) {
    std::string normalized_mac = NormalizeMac(mac_address);
    std::lock_guard<std::mutex> lock(mutex_);
    auto size_before = profiles_.size();
    profiles_.erase(std::remove_if(profiles_.begin(), profiles_.end(), [&](const DeviceProfile& profile) {
                        return profile.mac_address == normalized_mac;
                    }),
                    profiles_.end());
    if (profiles_.size() == size_before) {
        return false;
    }
    PersistProfilesLocked();
    return true;
}

bool DeviceRegistry::RemoveProfileById(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto size_before = profiles_.size();
    profiles_.erase(std::remove_if(profiles_.begin(), profiles_.end(), [&](const DeviceProfile& profile) {
                        return profile.device_id == device_id;
                    }),
                    profiles_.end());
    if (profiles_.size() == size_before) {
        return false;
    }
    PersistProfilesLocked();
    return true;
}

std::optional<DeviceProfile> DeviceRegistry::GetProfileByMac(const std::string& mac_address) const {
    std::string normalized_mac = NormalizeMac(mac_address);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(profiles_.begin(), profiles_.end(), [&](const DeviceProfile& profile) {
        return profile.mac_address == normalized_mac;
    });
    if (it == profiles_.end()) {
        return std::nullopt;
    }
    return *it;
}

std::optional<DeviceProfile> DeviceRegistry::GetProfileById(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::find_if(profiles_.begin(), profiles_.end(), [&](const DeviceProfile& profile) {
        return profile.device_id == device_id;
    });
    if (it == profiles_.end()) {
        return std::nullopt;
    }
    return *it;
}

void DeviceRegistry::UpdateSessions(const std::vector<DeviceRegistry::SessionInfo>& sessions) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
    std::string detected_active;
    for (auto session : sessions) {
        if (session.session_id.empty()) {
            continue;
        }
        if (session.is_active && detected_active.empty()) {
            detected_active = session.session_id;
        }
        sessions_.emplace(session.session_id, session);
    }

    if (!preferred_session_id_.empty()) {
        auto it = sessions_.find(preferred_session_id_);
        if (it == sessions_.end()) {
            preferred_session_id_.clear();
            PersistPreferredSessionLocked();
        }
    }

    if (preferred_session_id_.empty()) {
        preferred_session_id_ = !detected_active.empty() ? detected_active : (sessions.empty() ? std::string() : sessions.front().session_id);
        if (!preferred_session_id_.empty()) {
            PersistPreferredSessionLocked();
        }
    }

    for (auto& [session_id, info] : sessions_) {
        info.is_preferred = (session_id == preferred_session_id_);
    }
}

std::vector<DeviceRegistry::SessionInfo> DeviceRegistry::GetSessions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceRegistry::SessionInfo> sessions;
    sessions.reserve(sessions_.size());
    for (const auto& [_, info] : sessions_) {
        sessions.push_back(info);
    }
    std::sort(sessions.begin(), sessions.end(), [](const DeviceRegistry::SessionInfo& lhs, const DeviceRegistry::SessionInfo& rhs) {
        if (lhs.is_preferred != rhs.is_preferred) {
            return lhs.is_preferred && !rhs.is_preferred;
        }
        if (lhs.is_active != rhs.is_active) {
            return lhs.is_active && !rhs.is_active;
        }
        return lhs.session_id < rhs.session_id;
    });
    return sessions;
}

std::optional<DeviceRegistry::SessionInfo> DeviceRegistry::GetActiveSession() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!preferred_session_id_.empty()) {
        auto it = sessions_.find(preferred_session_id_);
        if (it != sessions_.end()) {
            return it->second;
        }
    }
    auto it = std::find_if(sessions_.begin(), sessions_.end(), [](const auto& entry) {
        return entry.second.is_active;
    });
    if (it != sessions_.end()) {
        return it->second;
    }
    if (!sessions_.empty()) {
        return sessions_.begin()->second;
    }
    return std::nullopt;
}

std::optional<DeviceRegistry::SessionInfo> DeviceRegistry::FindSession(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool DeviceRegistry::SetPreferredSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        ESP_LOGW(kTag, "Cannot set preferred session %s: session not found", session_id.c_str());
        return false;
    }
    preferred_session_id_ = session_id;
    for (auto& [id, info] : sessions_) {
        info.is_preferred = (id == preferred_session_id_);
    }
    PersistPreferredSessionLocked();
    return true;
}

void DeviceRegistry::LoadProfilesLocked() {
    Settings settings(kNamespace, false);
    std::string json = settings.GetString(kProfilesKey, "");
    if (json.empty()) {
        profiles_.clear();
        return;
    }

    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        ESP_LOGW(kTag, "Failed to parse stored profiles");
        profiles_.clear();
        return;
    }

    profiles_.clear();
    if (cJSON_IsArray(root)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            if (!cJSON_IsObject(item)) {
                continue;
            }
            profiles_.push_back(ParseProfile(item));
        }
    }
    cJSON_Delete(root);
}

void DeviceRegistry::PersistProfilesLocked() {
    cJSON* root = cJSON_CreateArray();
    for (const auto& profile : profiles_) {
        cJSON_AddItemToArray(root, SerializeProfile(profile));
    }
    char* json = cJSON_PrintUnformatted(root);
    Settings settings(kNamespace, true);
    settings.SetString(kProfilesKey, json != nullptr ? json : "");
    if (json != nullptr) {
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

void DeviceRegistry::PersistPreferredSessionLocked() {
    Settings settings(kNamespace, true);
    if (preferred_session_id_.empty()) {
        settings.EraseKey(kPreferredSessionKey);
    } else {
        settings.SetString(kPreferredSessionKey, preferred_session_id_);
    }
}

std::string DeviceRegistry::NormalizeMac(const std::string& mac_address) {
    return NormalizeMacAddress(mac_address);
}
