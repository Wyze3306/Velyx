#pragma once

#include <string>
#include <vector>

namespace velyx {

struct Account {
    std::string id;
    std::string label;
    std::string gamertag;
    std::string colorHex = "#3DDC84";
    std::string instanceId;
    std::string note;

    long long lastUsedMs = 0;
    long long totalPlaySeconds = 0;
};

class AccountStore {
public:
    static AccountStore& get();

    void load();
    void save() const;

    [[nodiscard]] const std::vector<Account>& all() const { return accounts_; }
    [[nodiscard]] Account* find(const std::string& id);
    [[nodiscard]] const Account* forInstance(const std::string& instanceId) const;

    Account& create(const std::string& label);
    bool remove(const std::string& id);

    bool bind(const std::string& accountId, const std::string& instanceId);
    void unbind(const std::string& accountId);

    void markUsed(const std::string& accountId);

    [[nodiscard]] std::vector<const Account*> recent() const;

private:
    AccountStore() = default;

    std::vector<Account> accounts_;
};

}
