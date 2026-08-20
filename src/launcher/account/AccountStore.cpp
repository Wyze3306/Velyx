#include "AccountStore.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

#include <json/json.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Strings.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Accounts";

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::filesystem::path storeFile() { return Paths::accounts() / "accounts.json"; }

}

AccountStore& AccountStore::get() {
    static AccountStore instance;
    return instance;
}

void AccountStore::load() {
    accounts_.clear();

    std::error_code ec;
    std::filesystem::create_directories(Paths::accounts(), ec);

    std::ifstream stream(storeFile());
    if (!stream) return;

    nlohmann::json document;
    try {
        stream >> document;
    } catch (const std::exception& e) {
        Log::warn(kLog, "accounts.json unreadable: {}", e.what());
        return;
    }
    if (!document.is_array()) return;

    for (const auto& entry : document) {
        Account account;
        account.id = entry.value("id", std::string{});
        account.label = entry.value("label", std::string("Compte"));
        account.gamertag = entry.value("gamertag", std::string{});
        account.colorHex = entry.value("color", std::string("#3DDC84"));
        account.instanceId = entry.value("instanceId", std::string{});
        account.note = entry.value("note", std::string{});
        account.lastUsedMs = entry.value("lastUsedMs", 0LL);
        account.totalPlaySeconds = entry.value("totalPlaySeconds", 0LL);

        if (!account.id.empty()) accounts_.push_back(std::move(account));
    }

    Log::info(kLog, "loaded {} account(s)", accounts_.size());
}

void AccountStore::save() const {
    nlohmann::json document = nlohmann::json::array();

    for (const Account& account : accounts_) {
        nlohmann::json entry;
        entry["id"] = account.id;
        entry["label"] = account.label;
        entry["gamertag"] = account.gamertag;
        entry["color"] = account.colorHex;
        entry["instanceId"] = account.instanceId;
        entry["note"] = account.note;
        entry["lastUsedMs"] = account.lastUsedMs;
        entry["totalPlaySeconds"] = account.totalPlaySeconds;
        document.push_back(entry);
    }

    std::error_code ec;
    std::filesystem::create_directories(Paths::accounts(), ec);

    std::ofstream stream(storeFile());
    if (stream) stream << document.dump(2);
}

Account* AccountStore::find(const std::string& id) {
    const auto it = std::ranges::find_if(accounts_, [&](const Account& a) { return a.id == id; });
    return it == accounts_.end() ? nullptr : &*it;
}

const Account* AccountStore::forInstance(const std::string& instanceId) const {
    const auto it = std::ranges::find_if(
        accounts_, [&](const Account& a) { return a.instanceId == instanceId; });
    return it == accounts_.end() ? nullptr : &*it;
}

Account& AccountStore::create(const std::string& label) {
    Account account;
    account.label = label.empty() ? "Compte" : label;

    account.id = strings::hashId(account.label + std::to_string(nowMs())).substr(0, 12);
    account.lastUsedMs = nowMs();

    accounts_.push_back(std::move(account));
    save();

    return accounts_.back();
}

bool AccountStore::remove(const std::string& id) {
    const size_t before = accounts_.size();
    std::erase_if(accounts_, [&](const Account& a) { return a.id == id; });

    if (accounts_.size() == before) return false;
    save();
    return true;
}

bool AccountStore::bind(const std::string& accountId, const std::string& instanceId) {
    Account* account = find(accountId);
    if (!account) return false;

    for (Account& other : accounts_) {
        if (other.id != accountId && other.instanceId == instanceId) other.instanceId.clear();
    }

    account->instanceId = instanceId;
    save();
    return true;
}

void AccountStore::unbind(const std::string& accountId) {
    if (Account* account = find(accountId)) {
        account->instanceId.clear();
        save();
    }
}

void AccountStore::markUsed(const std::string& accountId) {
    if (Account* account = find(accountId)) {
        account->lastUsedMs = nowMs();
        save();
    }
}

std::vector<const Account*> AccountStore::recent() const {
    std::vector<const Account*> result;
    result.reserve(accounts_.size());
    for (const Account& account : accounts_) result.push_back(&account);

    std::ranges::sort(result, [](const Account* a, const Account* b) {
        return a->lastUsedMs > b->lastUsedMs;
    });
    return result;
}

}
