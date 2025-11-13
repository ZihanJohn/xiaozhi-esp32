#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct DeviceProfile {
    std::string device_id;
    std::string mac_address;
    std::string label;
    std::string description;
    std::string transport_hint;
    bool allow_audio = true;
    bool allow_notifications = true;
    bool is_primary = false;
};

class DeviceRegistry {
public:
    struct SessionInfo {
        std::string session_id;
        std::string device_id;
        std::string label;
        std::string transport;
        bool supports_udp = false;
        bool supports_mcp = false;
        bool is_active = false;
        bool is_preferred = false;
    };

    static DeviceRegistry& GetInstance();
    DeviceRegistry(const DeviceRegistry&) = delete;
    DeviceRegistry& operator=(const DeviceRegistry&) = delete;

    std::vector<DeviceProfile> GetProfiles() const;
    bool AddOrUpdateProfile(const DeviceProfile& profile);
    bool RemoveProfileByMac(const std::string& mac_address);
    bool RemoveProfileById(const std::string& device_id);
    std::optional<DeviceProfile> GetProfileByMac(const std::string& mac_address) const;
    std::optional<DeviceProfile> GetProfileById(const std::string& device_id) const;

    void UpdateSessions(const std::vector<SessionInfo>& sessions);
    std::vector<SessionInfo> GetSessions() const;
    std::optional<SessionInfo> GetActiveSession() const;
    std::optional<SessionInfo> FindSession(const std::string& session_id) const;
    bool SetPreferredSession(const std::string& session_id);

private:
    DeviceRegistry();

    void LoadProfilesLocked();
    void PersistProfilesLocked();
    void PersistPreferredSessionLocked();
    static std::string NormalizeMac(const std::string& mac_address);

    mutable std::mutex mutex_;
    std::vector<DeviceProfile> profiles_;
    std::unordered_map<std::string, SessionInfo> sessions_;
    std::string preferred_session_id_;
};

#endif // DEVICE_REGISTRY_H
