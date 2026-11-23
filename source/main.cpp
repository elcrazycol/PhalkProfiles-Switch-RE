// =============================================================================
// Phalk Profiles for Nintendo Switch
// =============================================================================
// One source tree, two binaries:
//
//   make            -> PhalkProfiles.NRO   (interactive client)
//   make sysmodule  -> sysmodule NSP       (background uploader)
//
// The application exports the play log of every local profile, writes the data
// as JSON and uploads it to phalk.net/profiles.
//
// Design rules used throughout this file:
//   * every libnx call result is checked;
//   * services are only exited when they were initialized successfully;
//   * no exceptions can escape into libnx (the UI reports errors instead);
//   * strings coming from the system are sanitized to valid UTF-8;
//   * JSON is streamed to disk instead of being built in one huge string, so
//     applet mode (limited memory) cannot be killed by serialization.
// =============================================================================

#include <switch.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <exception>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#define PHALK_VERSION "1.3.0"

#define CONFIG_DIR "/config/PhalkProfiles"
#define CONFIG_PATH CONFIG_DIR "/config.txt"
#define DATA_DIR "/switch/PhalkProfiles"
#define EXPORT_PATH DATA_DIR "/export.json"
#define DIAG_PATH DATA_DIR "/pdm_diag.log"

#define SERVER_HOST "phalk.net"
#define SERVER_PORT "80"
#define SERVER_PATH "/profiles/json_curl.php"
// Used only when DNS is unavailable.
#define SERVER_FALLBACK_IP "147.79.94.55"

#ifdef PHALK_SYSMODULE
#define LOG_PATH DATA_DIR "/sysmodule.log"
#else
#define LOG_PATH DATA_DIR "/app.log"
#endif

#ifndef PHALK_SYSMODULE
#define PHALK_SYSMODULE_TID "00FF0000D4A9E73C"
#define SYS_CONTENT_DIR "/atmosphere/contents/" PHALK_SYSMODULE_TID
#define SYS_EXEFS_PATH SYS_CONTENT_DIR "/exefs.nsp"
#define SYS_TOOLBOX_PATH SYS_CONTENT_DIR "/toolbox.json"
#define SYS_BOOT2_FLAG SYS_CONTENT_DIR "/flags/boot2.flag"
#define SYSMODULE_SOURCE_PATH "romfs:/sysmodule/sysmodule.nsp"
#define TOOLBOX_SOURCE_PATH "romfs:/sysmodule/toolbox.json"
#endif


// =============================================================================
// Small shared helpers
// =============================================================================

static void ensureDir(const char* path)
{
    mkdir(path, 0777);
}

static void ensureDataDir()
{
    ensureDir("/switch");
    ensureDir(DATA_DIR);
}

static void logMsg(const char* format, ...)
{
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    std::ofstream log(LOG_PATH, std::ios::app);
    if (!log)
        return;

    const time_t now = time(nullptr);
    log << "[" << (long long)(now > 0 ? now : 0) << "] " << message << "\n";
}

static std::string resultToString(Result rc)
{
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "0x%08x", (unsigned int)rc);
    return std::string(buffer);
}

static bool isUtf8Continuation(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

// Copies at most maxLen bytes from a fixed-size system string (NACP title,
// account nickname, ...) and guarantees that the result is valid UTF-8.
//
// This matters: an unvalidated/unterminated NACP string used to end up in
// nlohmann::json::dump(), which aborts the whole process on bad UTF-8 when
// the binary is built with -fno-exceptions.
static std::string sanitizeUtf8(const char* data, size_t maxLen, bool* hadErrors = nullptr)
{
    std::string out;
    if (hadErrors != nullptr)
        *hadErrors = false;
    if (data == nullptr)
        return out;

    auto markInvalid = [&]() {
        out.push_back('?');
        if (hadErrors != nullptr)
            *hadErrors = true;
    };

    size_t i = 0;
    while (i < maxLen && data[i] != '\0') {
        const unsigned char c = (unsigned char)data[i];
        size_t len = 0;
        unsigned int cp = 0;

        if (c < 0x80) {
            len = 1;
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07u;
        } else {
            markInvalid();
            ++i;
            continue;
        }

        // Truncated multi-byte character at the end of the buffer.
        if (i + len > maxLen) {
            if (hadErrors != nullptr)
                *hadErrors = true;
            break;
        }

        bool valid = true;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char next = (unsigned char)data[i + k];
            if (next == '\0' || !isUtf8Continuation(next)) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (next & 0x3Fu);
        }

        if (!valid) {
            markInvalid();
            ++i;
            continue;
        }

        const bool overlong = (len == 2 && cp < 0x80) ||
                              (len == 3 && cp < 0x800) ||
                              (len == 4 && cp < 0x10000);
        const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
        if (overlong || surrogate || cp > 0x10FFFF) {
            markInvalid();
            ++i;
            continue;
        }

        out.append(data + i, len);
        i += len;
    }

    return out;
}


// =============================================================================
// Configuration
// =============================================================================

struct Config
{
    std::string username;
    std::string password;
};

static Config loadConfig()
{
    Config cfg;

    std::ifstream file(CONFIG_PATH, std::ios::binary);
    if (!file)
        return cfg;

    std::getline(file, cfg.username);
    std::getline(file, cfg.password);

    // UTF-8 BOM written by some Windows editors.
    if (cfg.username.size() >= 3 &&
        (unsigned char)cfg.username[0] == 0xEF &&
        (unsigned char)cfg.username[1] == 0xBB &&
        (unsigned char)cfg.username[2] == 0xBF) {
        cfg.username.erase(0, 3);
    }

    if (!cfg.username.empty() && cfg.username.back() == '\r')
        cfg.username.pop_back();
    if (!cfg.password.empty() && cfg.password.back() == '\r')
        cfg.password.pop_back();

    return cfg;
}

#ifndef PHALK_SYSMODULE
static bool saveConfig(const Config& cfg, std::string& error)
{
    ensureDir("/config");
    ensureDir(CONFIG_DIR);

    std::ofstream file(CONFIG_PATH, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot open config.txt for writing";
        return false;
    }

    file << cfg.username << "\n";
    file << cfg.password << "\n";
    file.flush();

    if (!file.good()) {
        error = "write to config.txt failed";
        return false;
    }

    return true;
}
#endif // PHALK_SYSMODULE


// =============================================================================
// Play data export
// =============================================================================

namespace PlayData
{

struct TitleInfo
{
    u64 id = 0;
    std::string name;
    u64 firstPlayed = 0;
    u64 lastPlayed = 0;
    u64 playtime = 0;   // seconds
    u32 launches = 0;
};

struct UserInfo
{
    std::string name;
    std::string id;
    std::vector<TitleInfo> titles;
};

struct ExportData
{
    u64 timestamp = 0;
    std::vector<UserInfo> users;
};

// Reused between titles: one 147 KB allocation instead of one per game.
static NsApplicationControlData g_controlData;
static bool g_nsReady = false;

static std::string fallbackName(u64 titleId)
{
    char id[20];
    snprintf(id, sizeof(id), "%016llx", (unsigned long long)titleId);
    return std::string("Unknown Game (") + id + ")";
}

static bool takeValidName(const NacpLanguageEntry* entry, std::string& out)
{
    if (entry == nullptr)
        return false;

    bool invalid = false;
    std::string name = sanitizeUtf8(entry->name, sizeof(entry->name), &invalid);
    if (invalid || name.empty())
        return false;

    // Reject control-character garbage (uninitialized NACP entries can contain
    // arbitrary non-zero bytes) and trim stray whitespace.
    for (unsigned char c : name) {
        if (c < 0x20)
            return false;
    }

    const size_t first = name.find_first_not_of(" \t");
    const size_t last = name.find_last_not_of(" \t");
    if (first == std::string::npos)
        return false;

    out = name.substr(first, last - first + 1);
    return true;
}

// Reads the title name from NACP. The system language entry can contain
// garbage (or a language the title simply did not fill in), so every language
// entry is validated and the first usable UTF-8 name wins.
static std::string titleName(u64 titleId)
{
    if (!g_nsReady)
        return fallbackName(titleId);

    memset(&g_controlData, 0, sizeof(g_controlData));

    u64 outSize = 0;
    Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, titleId,
                                            &g_controlData, sizeof(g_controlData), &outSize);
    if (R_FAILED(rc) || outSize == 0)
        return fallbackName(titleId);

    std::string name;

    NacpLanguageEntry* preferred = nullptr;
    if (R_SUCCEEDED(nacpGetLanguageEntry(&g_controlData.nacp, &preferred)) &&
        takeValidName(preferred, name)) {
        return name;
    }

    // NacpLanguageEntry lang[16] is at offset 0 in both old and new libnx
    // layouts (newer libnx wraps it in a union).
    const u8* languageBase = reinterpret_cast<const u8*>(&g_controlData.nacp);
    for (int i = 0; i < 16; ++i) {
        const NacpLanguageEntry* entry = reinterpret_cast<const NacpLanguageEntry*>(
            languageBase + (size_t)i * sizeof(NacpLanguageEntry));
        if (takeValidName(entry, name))
            return name;
    }

    return fallbackName(titleId);
}

static std::string uidToString(const AccountUid& uid)
{
    char buffer[40];
    snprintf(buffer, sizeof(buffer), "%016llx%016llx",
             (unsigned long long)uid.uid[0], (unsigned long long)uid.uid[1]);
    return std::string(buffer);
}

struct RawUserStats
{
    u64 launches = 0;
    u64 playtimeSeconds = 0;
    u64 firstPlayed = 0;
    u64 lastPlayed = 0;
};

struct RawTitleStats
{
    u64 id = 0;
    u64 events = 0;
    u64 launches = 0;
    u64 playtimeSeconds = 0;
    u64 firstPlayed = 0;
    u64 lastPlayed = 0;
    u32 policyMask = 0;
    u32 appletIdMask = 0;
    std::map<std::string, RawUserStats> users;
};

struct EventScanSummary
{
    s32 totalEntries = 0;
    s32 startIndex = 0;
    s32 endIndex = 0;
    u64 eventsRead = 0;
    u64 appletEvents = 0;
    u64 accountEvents = 0;
    u64 otherEvents = 0;
    std::set<std::string> accountUids;
    std::map<u64, RawTitleStats> titles;
};

// Reads the *raw* PDM play log. Unlike pdm:qry statistics queries, cmd 8 does
// not filter entries by PlayLogPolicy/AppletId, so titles hidden by the system
// (policy 1/3) are still visible here.
static EventScanSummary scanPlayEvents()
{
    EventScanSummary summary;

    Result rc = pdmqryGetAvailablePlayEventRange(
        &summary.totalEntries, &summary.startIndex, &summary.endIndex);
    if (R_FAILED(rc)) {
        logMsg("pdmqryGetAvailablePlayEventRange failed (%s)", resultToString(rc).c_str());
        return summary;
    }

    logMsg("PDM range: total=%d start=%d end=%d",
           summary.totalEntries, summary.startIndex, summary.endIndex);

    if (summary.totalEntries <= 0)
        return summary;

    struct SessionState
    {
        bool open = false;
        u64 focusStart = 0;
        std::string uid;
    };

    std::map<u64, SessionState> sessions;
    std::string activeUid;

    std::vector<PdmPlayEvent> events(512);
    s32 offset = summary.startIndex;
    s32 remaining = summary.totalEntries;

    for (s32 batch = 0; batch < 100000 && remaining > 0; ++batch) {
        s32 read = 0;
        rc = pdmqryQueryPlayEvent(offset, events.data(), (s32)events.size(), &read);
        if (R_FAILED(rc) || read <= 0)
            break;
        if (read > (s32)events.size())
            read = (s32)events.size();

        summary.eventsRead += (u64)read;

        for (s32 i = 0; i < read; ++i) {
            const PdmPlayEvent& event = events[(size_t)i];

            if (event.play_event_type == PdmPlayEventType_Applet) {
                ++summary.appletEvents;

                const auto& applet = event.event_data.applet;
                const u8 appletId = applet.applet_id;

                // Applications only. 0 is tolerated for the pre-16.0.0
                // conversion where libnx leaves the field zeroed.
                if (appletId != 0 && appletId != (u8)AppletId_application)
                    continue;

                const u64 titleId =
                    ((u64)applet.program_id[0] << 32) | (u64)applet.program_id[1];
                if (titleId == 0)
                    continue;

                RawTitleStats& title = summary.titles[titleId];
                title.id = titleId;
                ++title.events;
                title.policyMask |= (1u << (applet.log_policy & 0x1Fu));
                title.appletIdMask |= (1u << (appletId & 0x1Fu));

                u64 timestamp = event.timestamp_user;
                if (timestamp == 0)
                    timestamp = event.timestamp_steady;

                SessionState& session = sessions[titleId];

                auto addFocusTime = [&](u64 endTimestamp) {
                    if (session.focusStart != 0 && endTimestamp > session.focusStart) {
                        const u64 seconds = endTimestamp - session.focusStart;
                        title.playtimeSeconds += seconds;
                        if (!session.uid.empty())
                            title.users[session.uid].playtimeSeconds += seconds;
                    }
                    session.focusStart = 0;
                };

                switch (applet.event_type) {
                    case PdmAppletEventType_Launch:
                        if (session.open)
                            addFocusTime(timestamp);
                        session.open = true;
                        session.focusStart = 0;
                        session.uid = activeUid;

                        ++title.launches;
                        if (title.firstPlayed == 0 || timestamp < title.firstPlayed)
                            title.firstPlayed = timestamp;
                        if (!activeUid.empty()) {
                            RawUserStats& user = title.users[activeUid];
                            ++user.launches;
                            if (user.firstPlayed == 0 || timestamp < user.firstPlayed)
                                user.firstPlayed = timestamp;
                        }
                        break;

                    case PdmAppletEventType_InFocus:
                        if (session.open)
                            session.focusStart = timestamp;
                        break;

                    case PdmAppletEventType_OutOfFocus:
                    case PdmAppletEventType_OutOfFocus4:
                        if (session.open)
                            addFocusTime(timestamp);
                        break;

                    case PdmAppletEventType_Exit:
                    case PdmAppletEventType_Exit5:
                    case PdmAppletEventType_Exit6:
                        if (session.open) {
                            addFocusTime(timestamp);
                            session.open = false;
                            if (timestamp > title.lastPlayed)
                                title.lastPlayed = timestamp;
                            if (!session.uid.empty()) {
                                RawUserStats& user = title.users[session.uid];
                                if (timestamp > user.lastPlayed)
                                    user.lastPlayed = timestamp;
                            }
                        }
                        break;

                    default:
                        break;
                }
            } else if (event.play_event_type == PdmPlayEventType_Account) {
                ++summary.accountEvents;

                const auto& account = event.event_data.account;
                AccountUid uid{};
                uid.uid[0] = ((u64)account.uid[0] << 32) | account.uid[1];
                uid.uid[1] = ((u64)account.uid[2] << 32) | account.uid[3];

                const std::string uidString = uidToString(uid);
                summary.accountUids.insert(uidString);

                if (account.type == 0)
                    activeUid = uidString;
                else if (account.type == 1)
                    activeUid.clear();
            } else {
                ++summary.otherEvents;
            }
        }

        offset += read;
        remaining -= read;
    }

    logMsg("PDM scan: events=%llu applet=%llu account=%llu other=%llu titles=%lu",
           (unsigned long long)summary.eventsRead,
           (unsigned long long)summary.appletEvents,
           (unsigned long long)summary.accountEvents,
           (unsigned long long)summary.otherEvents,
           (unsigned long)summary.titles.size());

    return summary;
}

static bool collect(ExportData& out, std::string& error)
{
    out = ExportData{};

    const time_t now = time(nullptr);
    out.timestamp = now > 0 ? (u64)now : 0;

    bool accountReady = false;
    bool pdmReady = false;
    g_nsReady = false;

    // Closes only the services which were actually opened, and also does so if
    // an exception is thrown somewhere below.
    struct ServiceCleanup
    {
        bool* nsReady;
        bool* pdmReady;
        bool* accountReady;

        ~ServiceCleanup()
        {
            if (*nsReady) {
                nsExit();
                *nsReady = false;
            }
            if (*pdmReady) {
                pdmqryExit();
                *pdmReady = false;
            }
            if (*accountReady) {
                accountExit();
                *accountReady = false;
            }
        }
    } cleanup = { &g_nsReady, &pdmReady, &accountReady };
    (void)cleanup;

    // Application type is available to normal apps and to hbmenu. Fall back to
    // other service types in case the process is running in a restricted
    // context.
    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc))
        rc = accountInitialize(AccountServiceType_Administrator);
    if (R_FAILED(rc))
        rc = accountInitialize(AccountServiceType_System);
    if (R_FAILED(rc)) {
        error = "accountInitialize failed (" + resultToString(rc) + ")";
        return false;
    }
    accountReady = true;

    rc = pdmqryInitialize();
    if (R_FAILED(rc)) {
        error = "pdmqryInitialize failed (" + resultToString(rc) + ")";
        return false;
    }
    pdmReady = true;

    // Names are nice to have: export still works without the ns service.
    rc = nsInitialize();
    g_nsReady = R_SUCCEEDED(rc);
    if (!g_nsReady)
        logMsg("nsInitialize failed (%s), using generic game names", resultToString(rc).c_str());

    AccountUid userIds[ACC_USER_LIST_SIZE] = {};
    s32 userCount = 0;
    rc = accountListAllUsers(userIds, ACC_USER_LIST_SIZE, &userCount);
    if (R_FAILED(rc)) {
        error = "accountListAllUsers failed (" + resultToString(rc) + ")";
        return false;
    }

    if (userCount < 0)
        userCount = 0;
    if (userCount > ACC_USER_LIST_SIZE)
        userCount = ACC_USER_LIST_SIZE;

    // -------------------------------------------------------------------------
    // Raw PDM log: contains titles which the system hides from the normal
    // statistics queries (PlayLogPolicy 1/3).
    // -------------------------------------------------------------------------
    const EventScanSummary scan = scanPlayEvents();

    std::vector<u64> titleIds;
    titleIds.reserve(scan.titles.size());
    for (const auto& entry : scan.titles)
        titleIds.push_back(entry.first);

    // -------------------------------------------------------------------------
    // Per-user statistics and names.
    // -------------------------------------------------------------------------
    std::map<u64, std::string> nameCache;

    for (s32 i = 0; i < userCount; ++i) {
        UserInfo user;

        AccountProfile profile;
        if (R_SUCCEEDED(accountGetProfile(&profile, userIds[i]))) {
            AccountProfileBase base{};
            if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base)))
                user.name = sanitizeUtf8(base.nickname, sizeof(base.nickname));
            accountProfileClose(&profile);
        }

        const std::string userUid = uidToString(userIds[i]);
        user.id = userUid;

        for (u64 titleId : titleIds) {
            bool haveStats = false;
            PdmPlayStatistics stats{};

            Result statsRc = pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(
                titleId, userIds[i], false, &stats);
            if (R_SUCCEEDED(statsRc) && stats.total_launches > 0) {
                haveStats = true;
            } else {
                // flag=true additionally allows PlayLogPolicy 3 entries.
                PdmPlayStatistics withFlag{};
                Result flagRc = pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(
                    titleId, userIds[i], true, &withFlag);
                if (R_SUCCEEDED(flagRc) && withFlag.total_launches > 0) {
                    stats = withFlag;
                    haveStats = true;
                }
            }

            TitleInfo title;
            bool haveTitle = false;

            if (haveStats) {
                title.id = titleId;
                title.firstPlayed = stats.first_timestamp_user;
                title.lastPlayed = stats.last_timestamp_user;
                title.playtime = stats.playtime / 1000000000ULL; // ns -> seconds
                title.launches = stats.total_launches;
                haveTitle = true;
            } else {
                auto titleIt = scan.titles.find(titleId);
                if (titleIt != scan.titles.end()) {
                    auto userIt = titleIt->second.users.find(userUid);
                    if (userIt != titleIt->second.users.end()) {
                        const RawUserStats& raw = userIt->second;
                        if (raw.launches > 0 || raw.playtimeSeconds > 0) {
                            title.id = titleId;
                            title.firstPlayed = raw.firstPlayed;
                            title.lastPlayed = raw.lastPlayed;
                            title.playtime = raw.playtimeSeconds;
                            title.launches = (u32)raw.launches;
                            haveTitle = true;
                        }
                    }
                }
            }

            if (!haveTitle)
                continue;

            auto it = nameCache.find(titleId);
            if (it == nameCache.end())
                it = nameCache.emplace(titleId, titleName(titleId)).first;
            title.name = it->second;

            user.titles.push_back(std::move(title));
        }

        // Newest games first; the website does not depend on the order.
        std::sort(user.titles.begin(), user.titles.end(),
                  [](const TitleInfo& a, const TitleInfo& b) {
                      return a.lastPlayed > b.lastPlayed;
                  });

        out.users.push_back(std::move(user));
    }

    // -------------------------------------------------------------------------
    // Recovery pass. If the PDM account events were lost, raw sessions have no
    // user attached. Attribute such titles to the user with the most raw
    // activity so they are not silently dropped from the export.
    // -------------------------------------------------------------------------
    if (!out.users.empty() && !scan.titles.empty()) {
        std::set<u64> included;
        for (const UserInfo& user : out.users) {
            for (const TitleInfo& title : user.titles)
                included.insert(title.id);
        }

        std::map<std::string, u64> rawUserWeight;
        for (const auto& titleEntry : scan.titles) {
            for (const auto& userEntry : titleEntry.second.users)
                rawUserWeight[userEntry.first] += userEntry.second.playtimeSeconds + userEntry.second.launches;
        }

        s32 primary = 0;
        u64 bestWeight = 0;
        for (const auto& weightEntry : rawUserWeight) {
            if (weightEntry.second <= bestWeight)
                continue;
            for (s32 i = 0; i < userCount; ++i) {
                if (out.users[(size_t)i].id == weightEntry.first) {
                    primary = i;
                    bestWeight = weightEntry.second;
                    break;
                }
            }
        }

        UserInfo& primaryUser = out.users[(size_t)primary];

        for (const auto& titleEntry : scan.titles) {
            const u64 titleId = titleEntry.first;
            if (included.count(titleId))
                continue;

            const RawTitleStats& raw = titleEntry.second;
            if (raw.launches == 0)
                continue;

            u64 launches = raw.launches;
            u64 playtime = raw.playtimeSeconds;
            u64 firstPlayed = raw.firstPlayed;
            u64 lastPlayed = raw.lastPlayed;

            auto userIt = raw.users.find(primaryUser.id);
            if (userIt != raw.users.end() &&
                (userIt->second.launches > 0 || userIt->second.playtimeSeconds > 0)) {
                launches = userIt->second.launches;
                playtime = userIt->second.playtimeSeconds;
                firstPlayed = userIt->second.firstPlayed;
                lastPlayed = userIt->second.lastPlayed;
            }

            if (launches == 0)
                continue;

            auto nameIt = nameCache.find(titleId);
            if (nameIt == nameCache.end())
                nameIt = nameCache.emplace(titleId, titleName(titleId)).first;

            TitleInfo title;
            title.id = titleId;
            title.name = nameIt->second;
            title.firstPlayed = firstPlayed;
            title.lastPlayed = lastPlayed;
            title.playtime = playtime;
            title.launches = (u32)launches;

            primaryUser.titles.push_back(std::move(title));
            included.insert(titleId);

            logMsg("recovered raw title %016llx for user %s",
                   (unsigned long long)titleId, primaryUser.id.c_str());
        }

        std::sort(primaryUser.titles.begin(), primaryUser.titles.end(),
                  [](const TitleInfo& a, const TitleInfo& b) {
                      return a.lastPlayed > b.lastPlayed;
                  });
    }

    return true;
}

#ifndef PHALK_SYSMODULE
// Writes a human-readable dump of the raw PDM log and of the names/statistics
// which the export would use. The file is meant to be shared when a console's
// play activity does not match what the user expects.
static bool runDiagnostics(std::string& error)
{
    ensureDataDir();

    std::ofstream diag(DIAG_PATH, std::ios::binary | std::ios::trunc);
    if (!diag) {
        error = "cannot open pdm_diag.log";
        return false;
    }

    auto out = [&diag](const char* format, ...) {
        char buffer[2048];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        diag << buffer << "\n";
    };

    bool accountReady = false;
    bool pdmReady = false;
    g_nsReady = false;

    struct ServiceCleanup
    {
        bool* nsReady;
        bool* pdmReady;
        bool* accountReady;

        ~ServiceCleanup()
        {
            if (*nsReady) {
                nsExit();
                *nsReady = false;
            }
            if (*pdmReady) {
                pdmqryExit();
                *pdmReady = false;
            }
            if (*accountReady) {
                accountExit();
                *accountReady = false;
            }
        }
    } cleanup = { &g_nsReady, &pdmReady, &accountReady };
    (void)cleanup;

    const u32 hosVersion = hosversionGet();
    out("Phalk Profiles PDM diagnostics");
    out("firmware: %u.%u.%u (raw 0x%08x)",
        (unsigned)((hosVersion >> 16) & 0xFF),
        (unsigned)((hosVersion >> 8) & 0xFF),
        (unsigned)(hosVersion & 0xFF),
        (unsigned)hosVersion);
    out("time: %lld", (long long)time(nullptr));

    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc))
        rc = accountInitialize(AccountServiceType_Administrator);
    if (R_FAILED(rc))
        rc = accountInitialize(AccountServiceType_System);
    if (R_FAILED(rc)) {
        error = "accountInitialize failed (" + resultToString(rc) + ")";
        out("accountInitialize failed: %s", resultToString(rc).c_str());
        return false;
    }
    accountReady = true;

    rc = pdmqryInitialize();
    if (R_FAILED(rc)) {
        error = "pdmqryInitialize failed (" + resultToString(rc) + ")";
        out("pdmqryInitialize failed: %s", resultToString(rc).c_str());
        return false;
    }
    pdmReady = true;

    rc = nsInitialize();
    g_nsReady = R_SUCCEEDED(rc);
    out("nsInitialize: %s", resultToString(rc).c_str());

    AccountUid userIds[ACC_USER_LIST_SIZE] = {};
    s32 userCount = 0;
    rc = accountListAllUsers(userIds, ACC_USER_LIST_SIZE, &userCount);
    out("accountListAllUsers: %s, count=%d", resultToString(rc).c_str(), userCount);

    if (userCount < 0)
        userCount = 0;
    if (userCount > ACC_USER_LIST_SIZE)
        userCount = ACC_USER_LIST_SIZE;

    for (s32 i = 0; i < userCount; ++i) {
        std::string name;
        AccountProfile profile;
        if (R_SUCCEEDED(accountGetProfile(&profile, userIds[i]))) {
            AccountProfileBase base{};
            if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base)))
                name = sanitizeUtf8(base.nickname, sizeof(base.nickname));
            accountProfileClose(&profile);
        }
        out("user[%d]: %s  name=%s", i, uidToString(userIds[i]).c_str(), name.c_str());
    }

    const EventScanSummary scan = scanPlayEvents();
    out("range: total=%d start=%d end=%d", scan.totalEntries, scan.startIndex, scan.endIndex);
    out("events: read=%llu applet=%llu account=%llu other=%llu",
        (unsigned long long)scan.eventsRead,
        (unsigned long long)scan.appletEvents,
        (unsigned long long)scan.accountEvents,
        (unsigned long long)scan.otherEvents);
    out("account uids seen: %lu", (unsigned long)scan.accountUids.size());
    for (const std::string& uid : scan.accountUids)
        out("  account uid: %s", uid.c_str());

    out("unique applet titles: %lu", (unsigned long)scan.titles.size());

    for (const auto& entry : scan.titles) {
        const RawTitleStats& title = entry.second;

        out("title %016llx events=%llu launches=%llu playtime=%llu policies=0x%x appletIds=0x%x first=%llu last=%llu",
            (unsigned long long)title.id,
            (unsigned long long)title.events,
            (unsigned long long)title.launches,
            (unsigned long long)title.playtimeSeconds,
            title.policyMask, title.appletIdMask,
            (unsigned long long)title.firstPlayed,
            (unsigned long long)title.lastPlayed);

        for (const auto& userEntry : title.users) {
            const RawUserStats& raw = userEntry.second;
            out("  raw user=%s launches=%llu playtime=%llu first=%llu last=%llu",
                userEntry.first.c_str(),
                (unsigned long long)raw.launches,
                (unsigned long long)raw.playtimeSeconds,
                (unsigned long long)raw.firstPlayed,
                (unsigned long long)raw.lastPlayed);
        }

        for (s32 i = 0; i < userCount; ++i) {
            PdmPlayStatistics stats0{};
            Result rc0 = pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(
                title.id, userIds[i], false, &stats0);
            PdmPlayStatistics stats1{};
            Result rc1 = pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(
                title.id, userIds[i], true, &stats1);

            out("  stats user=%s flag0=%s launches=%u playtime=%llu | flag1=%s launches=%u playtime=%llu",
                uidToString(userIds[i]).c_str(),
                resultToString(rc0).c_str(),
                (unsigned)stats0.total_launches,
                (unsigned long long)(stats0.playtime / 1000000000ULL),
                resultToString(rc1).c_str(),
                (unsigned)stats1.total_launches,
                (unsigned long long)(stats1.playtime / 1000000000ULL));
        }

        if (!g_nsReady)
            continue;

        memset(&g_controlData, 0, sizeof(g_controlData));
        u64 outSize = 0;
        Result nacpRc = nsGetApplicationControlData(NsApplicationControlSource_Storage, title.id,
                                                    &g_controlData, sizeof(g_controlData), &outSize);
        out("  nacp: %s outSize=%llu playLogPolicy=%u",
            resultToString(nacpRc).c_str(),
            (unsigned long long)outSize,
            (unsigned)g_controlData.nacp.play_log_policy);
        if (R_FAILED(nacpRc))
            continue;

        const u8* languageBase = reinterpret_cast<const u8*>(&g_controlData.nacp);
        for (int lang = 0; lang < 16; ++lang) {
            const NacpLanguageEntry* langEntry = reinterpret_cast<const NacpLanguageEntry*>(
                languageBase + (size_t)lang * sizeof(NacpLanguageEntry));

            bool invalid = false;
            std::string name = sanitizeUtf8(langEntry->name, sizeof(langEntry->name), &invalid);
            out("  nacp lang[%02d]: valid=%d name=\"%s\" bytes=%02x %02x %02x %02x",
                lang, invalid ? 0 : 1, name.c_str(),
                (unsigned)(unsigned char)langEntry->name[0],
                (unsigned)(unsigned char)langEntry->name[1],
                (unsigned)(unsigned char)langEntry->name[2],
                (unsigned)(unsigned char)langEntry->name[3]);
        }
    }

    diag.flush();
    if (!diag.good()) {
        error = "write to pdm_diag.log failed";
        return false;
    }

    return true;
}
#endif // PHALK_SYSMODULE

} // namespace PlayData


// =============================================================================
// JSON export (streamed, so memory usage stays flat)
// =============================================================================

static void writeJsonString(std::ostream& out, const std::string& value)
{
    out.put('"');
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = (unsigned char)value[i];
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b";  break;
            case '\f': out << "\\f";  break;
            case '\n': out << "\\n";  break;
            case '\r': out << "\\r";  break;
            case '\t': out << "\\t";  break;
            default:
                if (c < 0x20) {
                    char escaped[8];
                    snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out << escaped;
                } else {
                    out.put((char)c);
                }
                break;
        }
    }
    out.put('"');
}

static bool writeExportFile(const PlayData::ExportData& data, std::string& error)
{
    ensureDataDir();

    std::ofstream file(EXPORT_PATH, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot open export.json for writing";
        return false;
    }

    file << "{\n";
    file << "    \"exportString\": \"Exported using PhalkProfiles\",\n";
    file << "    \"exportTimestamp\": " << data.timestamp << ",\n";
    file << "    \"users\": [";
    if (!data.users.empty())
        file << "\n";

    for (size_t i = 0; i < data.users.size(); ++i) {
        const PlayData::UserInfo& user = data.users[i];
        if (i != 0)
            file << ",\n";

        file << "        {\n";
        file << "            \"name\": ";
        writeJsonString(file, user.name);
        file << ",\n";
        file << "            \"id\": ";
        writeJsonString(file, user.id);
        file << ",\n";
        file << "            \"titles\": [";
        if (!user.titles.empty())
            file << "\n";

        for (size_t j = 0; j < user.titles.size(); ++j) {
            const PlayData::TitleInfo& title = user.titles[j];
            if (j != 0)
                file << ",\n";

            char titleId[20];
            snprintf(titleId, sizeof(titleId), "%016llx", (unsigned long long)title.id);

            file << "                {\n";
            file << "                    \"id\": ";
            writeJsonString(file, titleId);
            file << ",\n";
            file << "                    \"name\": ";
            writeJsonString(file, title.name);
            file << ",\n";
            file << "                    \"summary\": {\n";
            file << "                        \"firstPlayed\": " << title.firstPlayed << ",\n";
            file << "                        \"lastPlayed\": " << title.lastPlayed << ",\n";
            file << "                        \"playtime\": " << title.playtime << ",\n";
            file << "                        \"launches\": " << title.launches << "\n";
            file << "                    }\n";
            file << "                }";
        }

        if (!user.titles.empty())
            file << "\n            ";
        file << "]\n";
        file << "        }";
    }

    if (!data.users.empty())
        file << "\n    ";
    file << "]\n";
    file << "}\n";

    file.flush();
    if (!file.good()) {
        error = "write to export.json failed";
        return false;
    }

    return true;
}


// =============================================================================
// Upload
// =============================================================================

namespace Upload
{

static bool sendAll(int sock, const void* data, size_t size)
{
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t sent = send(sock, ptr, size, 0);
        if (sent <= 0)
            return false;
        ptr += sent;
        size -= (size_t)sent;
    }
    return true;
}

static int connectToServer(std::string& error)
{
    auto openSocket = [](const struct sockaddr* address, socklen_t addressLen) -> int {
        const int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return -1;

        timeval timeout{};
        timeout.tv_sec = 20;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        if (connect(sock, address, addressLen) != 0) {
            close(sock);
            return -1;
        }
        return sock;
    };

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (getaddrinfo(SERVER_HOST, SERVER_PORT, &hints, &result) == 0 && result != nullptr) {
        for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
            const int sock = openSocket(entry->ai_addr, (socklen_t)entry->ai_addrlen);
            if (sock >= 0) {
                freeaddrinfo(result);
                return sock;
            }
        }
        freeaddrinfo(result);
    }

    // DNS failed: use the known address as a fallback.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(80);
    address.sin_addr.s_addr = inet_addr(SERVER_FALLBACK_IP);

    const int sock = openSocket(reinterpret_cast<const struct sockaddr*>(&address), sizeof(address));
    if (sock < 0) {
        error = "connection to " SERVER_HOST " failed";
        return -1;
    }
    return sock;
}

static bool upload(const Config& cfg, std::string& error)
{
    if (cfg.username.empty() || cfg.password.empty()) {
        error = "username or password is not set";
        return false;
    }

    struct stat fileStat;
    if (stat(EXPORT_PATH, &fileStat) != 0 || !S_ISREG(fileStat.st_mode)) {
        error = "export.json is missing";
        return false;
    }

    std::ifstream file(EXPORT_PATH, std::ios::binary);
    if (!file) {
        error = "cannot open export.json";
        return false;
    }

    const char* boundary = "----PhalkProfilesBoundary7MA4YWxkTrZu0gW";

    const std::string start =
        std::string("--") + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"user_name\"\r\n\r\n" + cfg.username + "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"password\"\r\n\r\n" + cfg.password + "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"json_file\"; filename=\"export.json\"\r\n"
        "Content-Type: application/json\r\n\r\n";

    const std::string end = std::string("\r\n--") + boundary + "--\r\n";
    const unsigned long long contentLength =
        (unsigned long long)start.size() + (unsigned long long)fileStat.st_size +
        (unsigned long long)end.size();

    char header[1024];
    const int headerLength = snprintf(
        header, sizeof(header),
        "POST " SERVER_PATH " HTTP/1.0\r\n"
        "Host: " SERVER_HOST "\r\n"
        "User-Agent: PhalkProfiles/" PHALK_VERSION "\r\n"
        "Content-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %llu\r\n"
        "Connection: close\r\n\r\n",
        boundary, contentLength);
    if (headerLength <= 0 || headerLength >= (int)sizeof(header)) {
        error = "request header is too long";
        return false;
    }

    const int sock = connectToServer(error);
    if (sock < 0)
        return false;

    logMsg("upload started (%llu bytes)", contentLength);

    bool ok = sendAll(sock, header, (size_t)headerLength) &&
              sendAll(sock, start.data(), start.size());

    char buffer[8192];
    while (ok && file.good()) {
        file.read(buffer, sizeof(buffer));
        const std::streamsize count = file.gcount();
        if (count > 0)
            ok = sendAll(sock, buffer, (size_t)count);
    }
    if (ok)
        ok = sendAll(sock, end.data(), end.size());

    std::string response;
    if (ok) {
        char chunk[2048];
        for (int i = 0; i < 8 && response.size() < 16384; ++i) {
            const ssize_t received = recv(sock, chunk, sizeof(chunk), 0);
            if (received <= 0)
                break;
            response.append(chunk, (size_t)received);
        }
    }

    close(sock);

    if (!ok) {
        error = "sending the request failed";
        logMsg("upload failed: %s", error.c_str());
        return false;
    }

    int statusCode = 0;
    if (response.compare(0, 5, "HTTP/") == 0) {
        const size_t space = response.find(' ');
        if (space != std::string::npos)
            statusCode = atoi(response.c_str() + space + 1);
    }

    const size_t bodyPos = response.find("\r\n\r\n");
    const char* body = bodyPos == std::string::npos ? "" : response.c_str() + bodyPos + 4;
    logMsg("upload response: HTTP %d, body: %.300s", statusCode, body);

    if (statusCode < 200 || statusCode >= 300) {
        if (statusCode == 0)
            error = "invalid server response";
        else {
            char text[32];
            snprintf(text, sizeof(text), "HTTP %d", statusCode);
            error = text;
        }
        return false;
    }

    return true;
}

} // namespace Upload


// =============================================================================
// Application (NRO)
// =============================================================================

#ifndef PHALK_SYSMODULE

static bool fileExists(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool readableFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

static bool sameFile(const char* first, const char* second)
{
    std::ifstream a(first, std::ios::binary);
    std::ifstream b(second, std::ios::binary);
    if (!a || !b)
        return false;

    char aBuffer[4096];
    char bBuffer[4096];

    while (true) {
        a.read(aBuffer, sizeof(aBuffer));
        b.read(bBuffer, sizeof(bBuffer));

        const std::streamsize aCount = a.gcount();
        const std::streamsize bCount = b.gcount();

        if (aCount != bCount)
            return false;
        if (aCount == 0)
            return true;
        if (!std::equal(aBuffer, aBuffer + aCount, bBuffer))
            return false;
    }
}

static bool copyFile(const char* source, const char* destination)
{
    std::ifstream in(source, std::ios::binary);
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!in || !out)
        return false;

    char buffer[4096];
    while (in) {
        in.read(buffer, sizeof(buffer));
        const std::streamsize count = in.gcount();
        if (count > 0)
            out.write(buffer, count);
    }

    out.flush();
    return out.good();
}

static bool isAppletMode()
{
    const AppletType type = appletGetAppletType();
    return type != AppletType_Application && type != AppletType_SystemApplication;
}

// -----------------------------------------------------------------------------
// Sysmodule management
// -----------------------------------------------------------------------------

struct SysmoduleState
{
    bool installed = false;
    bool current = false;
    bool enabled = false;
};

static SysmoduleState probeSysmoduleState()
{
    SysmoduleState state;

    state.installed = fileExists(SYS_EXEFS_PATH) && fileExists(SYS_TOOLBOX_PATH);
    state.enabled = fileExists(SYS_BOOT2_FLAG);

    if (state.installed && readableFile(SYSMODULE_SOURCE_PATH))
        state.current = sameFile(SYSMODULE_SOURCE_PATH, SYS_EXEFS_PATH);

    return state;
}

static bool installSysmodule(std::string& error)
{
    if (!readableFile(SYSMODULE_SOURCE_PATH)) {
        error = "sysmodule payload is missing from RomFS";
        return false;
    }
    if (!readableFile(TOOLBOX_SOURCE_PATH)) {
        error = "toolbox.json is missing from RomFS";
        return false;
    }

    ensureDir("/atmosphere");
    ensureDir("/atmosphere/contents");
    ensureDir(SYS_CONTENT_DIR);
    ensureDir(SYS_CONTENT_DIR "/flags");

    // Always overwrite: this directory belongs to our TID and the payload is
    // shipped inside this very NRO. This also updates an outdated install.
    if (!copyFile(SYSMODULE_SOURCE_PATH, SYS_EXEFS_PATH)) {
        error = "cannot write exefs.nsp";
        return false;
    }
    if (!copyFile(TOOLBOX_SOURCE_PATH, SYS_TOOLBOX_PATH)) {
        error = "cannot write toolbox.json";
        return false;
    }
    if (!sameFile(SYSMODULE_SOURCE_PATH, SYS_EXEFS_PATH)) {
        error = "verification of exefs.nsp failed";
        return false;
    }
    if (!fileExists(SYS_TOOLBOX_PATH)) {
        error = "verification of toolbox.json failed";
        return false;
    }

    return true;
}

static bool setSysmoduleEnabled(bool enabled, std::string& error)
{
    if (enabled) {
        ensureDir(SYS_CONTENT_DIR "/flags");

        std::ofstream flag(SYS_BOOT2_FLAG, std::ios::binary | std::ios::trunc);
        if (!flag) {
            error = "cannot create boot2.flag";
            return false;
        }
        return flag.good();
    }

    if (std::remove(SYS_BOOT2_FLAG) != 0 && errno != ENOENT) {
        error = "cannot remove boot2.flag";
        return false;
    }
    return true;
}

static bool removeInstalledSysmodule(std::string& error)
{
    if (fileExists(SYS_BOOT2_FLAG)) {
        error = "sysmodule is still enabled";
        return false;
    }

    if (std::remove(SYS_EXEFS_PATH) != 0 && errno != ENOENT) {
        error = "cannot remove exefs.nsp";
        return false;
    }
    std::remove(SYS_TOOLBOX_PATH);
    std::remove(SYS_BOOT2_FLAG);
    rmdir(SYS_CONTENT_DIR "/flags");
    rmdir(SYS_CONTENT_DIR);

    return true;
}

static void handleSysmoduleAction(SysmoduleState& state, std::string& status, bool& restartRequired)
{
    std::string error;

    if (!state.installed) {
        // Copy the payload but leave it disabled: enabling is an explicit,
        // separate action so a bad build can never start on the next boot.
        if (installSysmodule(error) && setSysmoduleEnabled(false, error)) {
            status = "Sysmodule installed but disabled.";
            logMsg("sysmodule installed (disabled)");
        } else {
            status = "Install failed: " + error;
            logMsg("sysmodule install failed: %s", error.c_str());
        }
    } else if (!state.current) {
        // Updating an enabled sysmodule deliberately disables it. The user has
        // to enable it again after verifying that the build works.
        if (installSysmodule(error) && setSysmoduleEnabled(false, error)) {
            status = "Sysmodule updated and disabled.";
            logMsg("sysmodule updated (disabled)");
        } else {
            status = "Update failed: " + error;
            logMsg("sysmodule update failed: %s", error.c_str());
        }
    } else if (!state.enabled) {
        if (setSysmoduleEnabled(true, error)) {
            status = "Sysmodule enabled. Reboot required.";
            restartRequired = true;
            logMsg("sysmodule enabled");
        } else {
            status = "Enable failed: " + error;
            logMsg("sysmodule enable failed: %s", error.c_str());
        }
    } else {
        if (setSysmoduleEnabled(false, error) && removeInstalledSysmodule(error)) {
            status = "Sysmodule removed. Reboot required.";
            restartRequired = true;
            logMsg("sysmodule removed");
        } else {
            status = "Remove failed: " + error;
            logMsg("sysmodule remove failed: %s", error.c_str());
        }
    }

    state = probeSysmoduleState();
}

// -----------------------------------------------------------------------------
// UI
// -----------------------------------------------------------------------------

static std::string ellipsize(const std::string& text, size_t maxLength)
{
    if (text.size() <= maxLength)
        return text;
    if (maxLength <= 3)
        return text.substr(0, maxLength);
    return text.substr(0, maxLength - 3) + "...";
}

static std::string askText(const char* guide, bool password)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return std::string();

    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide);
    if (password)
        swkbdConfigSetPasswordFlag(&kbd, true);

    char buffer[512] = {};
    const Result rc = swkbdShow(&kbd, buffer, sizeof(buffer));
    swkbdClose(&kbd);

    if (R_FAILED(rc))
        return std::string();

    return std::string(buffer);
}

static std::string sysmoduleLabel(const SysmoduleState& state)
{
    if (!state.installed)
        return "Install sysmodule (disabled)";
    if (!state.current)
        return "Update sysmodule (disables it)";
    if (!state.enabled)
        return "Enable sysmodule (experimental)";
    return "Disable & remove sysmodule";
}

static std::string sysmoduleStatusText(const SysmoduleState& state, bool restartRequired)
{
    std::string text;

    if (!state.installed)
        text = "not installed";
    else if (!state.current)
        text = "installed, outdated";
    else if (state.enabled)
        text = "installed, enabled";
    else
        text = "installed, disabled";

    if (restartRequired)
        text += " (reboot required)";

    return text;
}

static void drawMainScreen(const Config& cfg, const std::string& status, int selected,
                           bool restartRequired, const SysmoduleState& state, bool appletMode)
{
    const char* reset = "\x1b[0m";
    const char* green = "\x1b[32m";
    const char* red = "\x1b[31m";
    const char* yellow = "\x1b[33m";

    consoleClear();

    printf("PHALK PROFILES  v%s\n", PHALK_VERSION);
    printf("========================================\n\n");

    const char* statusColor = green;
    if (status.find("failed") != std::string::npos ||
        status.find("Failed") != std::string::npos)
        statusColor = red;
    else if (status.find("...") != std::string::npos)
        statusColor = yellow;

    printf("[ Status: %s%s%s ]\n\n",
           statusColor, ellipsize(status, 52).c_str(), reset);

    printf("  User:     %s\n",
           cfg.username.empty() ? "(not set)" : ellipsize(cfg.username, 32).c_str());
    printf("  Password: %s\n\n",
           cfg.password.empty() ? "(not set)" : "********");

    const std::string sysLabel = sysmoduleLabel(state);
    const char* items[] = {
        "Export & upload",
        "Set username",
        "Set password",
        sysLabel.c_str(),
        "PDM diagnostics",
        "View log",
        "Quit",
    };

    for (int i = 0; i < 7; ++i)
        printf("  %c %s\n", selected == i ? '>' : ' ', items[i]);

    printf("\n[ Sysmodule: %s ]\n", sysmoduleStatusText(state, restartRequired).c_str());

    if (appletMode) {
        printf("\n[ Applet mode: limited memory.\n"
               "  Launch through title override if export fails. ]\n");
    }

    printf("\n  Log: " LOG_PATH "\n");

    consoleUpdate(nullptr);
}

static void showLogScreen()
{
    consoleClear();
    printf("Log: " LOG_PATH "\n\n");

    std::vector<std::string> lines;
    std::ifstream file(LOG_PATH);
    std::string line;

    while (std::getline(file, line)) {
        lines.push_back(line);
        if (lines.size() > 16)
            lines.erase(lines.begin());
    }

    if (lines.empty())
        printf("(empty)\n");

    for (const std::string& entry : lines)
        printf("%s\n", ellipsize(entry, 78).c_str());

    printf("\nPress A or B to return\n");
    consoleUpdate(nullptr);
}

static void waitForBackButton(PadState& pad)
{
    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (down & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus))
            break;
        svcSleepThread(16'000'000ULL);
    }
}

static bool hasInternetConnection()
{
    // Give the connection a few seconds to settle before giving up.
    for (int attempt = 0; attempt < 6; ++attempt) {
        NifmInternetConnectionType type;
        u32 strength = 0;
        NifmInternetConnectionStatus connectionStatus = NifmInternetConnectionStatus_ConnectingUnknown1;

        if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &connectionStatus)))
            return true; // Do not block the upload on a status query failure.

        if (connectionStatus == NifmInternetConnectionStatus_Connected)
            return true;

        svcSleepThread(500'000'000ULL);
    }

    return false;
}

int main()
{
    consoleInit(nullptr);
    ensureDataDir();

    const Result romfsResult = romfsInit();
    const Result socketResult = socketInitializeDefault();

    Result nifmResult = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(nifmResult))
        nifmResult = nifmInitialize(NifmServiceType_System);
    const bool nifmReady = R_SUCCEEDED(nifmResult);

    logMsg("application started (romfs=%s socket=%s nifm=%s)",
           resultToString(romfsResult).c_str(),
           resultToString(socketResult).c_str(),
           resultToString(nifmResult).c_str());

    Config cfg = loadConfig();
    SysmoduleState sysmoduleState = probeSysmoduleState();
    const bool appletMode = isAppletMode();

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    std::string status = "Ready";
    int selected = 0;
    bool restartRequired = false;
    bool screenDirty = true;

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus)
            break;

        if (down & HidNpadButton_Up) {
            selected = (selected + 6) % 7;
            screenDirty = true;
        }
        if (down & HidNpadButton_Down) {
            selected = (selected + 1) % 7;
            screenDirty = true;
        }

        if (down & HidNpadButton_A) {
            if (selected == 0) {
                status = "Exporting...";
                drawMainScreen(cfg, status, selected, restartRequired, sysmoduleState, appletMode);

                try {
                PlayData::ExportData data;
                std::string error;

                if (!PlayData::collect(data, error)) {
                    status = "Export failed: " + error;
                    logMsg("export failed: %s", error.c_str());
                } else {
                    if (!writeExportFile(data, error)) {
                        status = "Export failed: " + error;
                        logMsg("export write failed: %s", error.c_str());
                    } else {
                        size_t titleCount = 0;
                        for (const PlayData::UserInfo& user : data.users)
                            titleCount += user.titles.size();

                        logMsg("export done: %lu users, %lu titles",
                               (unsigned long)data.users.size(),
                               (unsigned long)titleCount);

                        if (cfg.username.empty() || cfg.password.empty()) {
                            status = "Exported. Set credentials.";
                        } else if (nifmReady && !hasInternetConnection()) {
                            status = "No internet connection.";
                        } else {
                            status = "Uploading...";
                            drawMainScreen(cfg, status, selected, restartRequired,
                                           sysmoduleState, appletMode);

                            if (Upload::upload(cfg, error)) {
                                char summary[96];
                                snprintf(summary, sizeof(summary),
                                         "Success! %lu users, %lu games",
                                         (unsigned long)data.users.size(),
                                         (unsigned long)titleCount);
                                status = summary;
                                logMsg("upload ok");
                            } else {
                                status = "Upload failed: " + error;
                                logMsg("upload failed: %s", error.c_str());
                            }
                        }
                    }
                }
                } catch (const std::exception& exception) {
                    status = std::string("Error: ") + exception.what();
                    logMsg("unhandled exception during export/upload: %s", exception.what());
                } catch (...) {
                    status = "Error: unknown exception";
                    logMsg("unhandled unknown exception during export/upload");
                }
                screenDirty = true;
            } else if (selected == 1) {
                const std::string value = askText("Phalk username", false);
                if (!value.empty()) {
                    cfg.username = value;
                    std::string error;
                    if (saveConfig(cfg, error)) {
                        status = "Username saved.";
                        logMsg("username saved");
                    } else {
                        status = "Save failed: " + error;
                    }
                }
                screenDirty = true;
            } else if (selected == 2) {
                const std::string value = askText("Phalk password", true);
                if (!value.empty()) {
                    cfg.password = value;
                    std::string error;
                    if (saveConfig(cfg, error)) {
                        status = "Password saved.";
                        logMsg("password saved");
                    } else {
                        status = "Save failed: " + error;
                    }
                }
                screenDirty = true;
            } else if (selected == 3) {
                status = "Working...";
                drawMainScreen(cfg, status, selected, restartRequired, sysmoduleState, appletMode);
                handleSysmoduleAction(sysmoduleState, status, restartRequired);
                screenDirty = true;
            } else if (selected == 4) {
                status = "Running diagnostics...";
                drawMainScreen(cfg, status, selected, restartRequired, sysmoduleState, appletMode);

                std::string error;
                if (PlayData::runDiagnostics(error))
                    status = "Diagnostics saved to pdm_diag.log";
                else
                    status = "Diagnostics failed: " + error;
                screenDirty = true;
            } else if (selected == 5) {
                showLogScreen();
                waitForBackButton(pad);
                screenDirty = true;
            } else if (selected == 6) {
                break;
            }
        }

        if (screenDirty) {
            drawMainScreen(cfg, status, selected, restartRequired, sysmoduleState, appletMode);
            screenDirty = false;
        }

        svcSleepThread(16'000'000ULL);
    }

    logMsg("application exit");

    if (nifmReady)
        nifmExit();
    socketExit();
    romfsExit();
    consoleExit(nullptr);

    return 0;
}

#else // PHALK_SYSMODULE

// =============================================================================
// Background sysmodule
// =============================================================================

extern "C" {

u32 __nx_applet_type = AppletType_None;
// Keep this low: sysmodules keep their FS sessions for the whole session, and
// grabbing too many at boot can starve applets (hbmenu/Album) of FS sessions.
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void)
{
    static u8 innerHeap[0x200000]; // 2 MiB: export data + NACP buffer
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    fake_heap_start = innerHeap;
    fake_heap_end = innerHeap + sizeof(innerHeap);
}

void __libnx_init_time(void);

void __appInit(void)
{
    if (R_FAILED(smInitialize()))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    // A sysmodule does not inherit the firmware version from an applet.
    Result rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion firmware{};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&firmware))) {
            hosversionSet(MAKEHOSVERSION(firmware.major, firmware.minor, firmware.micro));
            logMsg("firmware: %u.%u.%u",
                   firmware.major, firmware.minor, firmware.micro);
        }
        setsysExit();
    } else {
        logMsg("setsysInitialize failed (%s)", resultToString(rc).c_str());
    }

    if (R_FAILED(fsInitialize()))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    fsdevMountSdmc();

    // The time service is initialized later, only when an upload actually
    // runs. Holding its sessions from boot is unnecessary and can interfere
    // with applets on some setups.
}

void __appExit(void)
{
    fsdevUnmountAll();
    fsExit();
    smExit();
}

} // extern "C"

static bool runExportAndUpload(const Config& cfg)
{
    PlayData::ExportData data;
    std::string error;

    if (!PlayData::collect(data, error)) {
        logMsg("export failed: %s", error.c_str());
        return false;
    }

    size_t titleCount = 0;
    for (const PlayData::UserInfo& user : data.users)
        titleCount += user.titles.size();

    if (!writeExportFile(data, error)) {
        logMsg("export write failed: %s", error.c_str());
        return false;
    }

    logMsg("export done: %lu users, %lu titles",
           (unsigned long)data.users.size(),
           (unsigned long)titleCount);

    if (!Upload::upload(cfg, error)) {
        logMsg("upload failed: %s", error.c_str());
        return false;
    }

    logMsg("upload ok");
    return true;
}

static void uploadCurrentProfiles()
{
    logMsg("upload requested");

    Config cfg = loadConfig();
    if (cfg.username.empty() || cfg.password.empty()) {
        logMsg("upload skipped: credentials are not set");
        return;
    }

    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc))
        rc = nifmInitialize(NifmServiceType_System);

    const bool nifmReady = R_SUCCEEDED(rc);
    if (nifmReady) {
        bool connected = false;

        for (int attempt = 0; attempt < 20; ++attempt) {
            NifmInternetConnectionType type;
            u32 strength = 0;
            NifmInternetConnectionStatus connectionStatus =
                NifmInternetConnectionStatus_ConnectingUnknown1;

            rc = nifmGetInternetConnectionStatus(&type, &strength, &connectionStatus);
            if (R_SUCCEEDED(rc) && connectionStatus == NifmInternetConnectionStatus_Connected) {
                connected = true;
                break;
            }
            svcSleepThread(500'000'000ULL);
        }

        if (!connected) {
            logMsg("upload skipped: no network connection");
            nifmExit();
            return;
        }
    }

    SocketInitConfig socketConfig = *socketGetDefaultInitConfig();
    socketConfig.tcp_tx_buf_size = 0x4000;
    socketConfig.tcp_rx_buf_size = 0x4000;
    socketConfig.tcp_tx_buf_max_size = 0x4000;
    socketConfig.tcp_rx_buf_max_size = 0x4000;
    socketConfig.udp_tx_buf_size = 0x2400;
    socketConfig.udp_rx_buf_size = 0x4000;
    socketConfig.sb_efficiency = 1;
    socketConfig.num_bsd_sessions = 1;
    socketConfig.bsd_service_type = BsdServiceType_Auto;

    rc = socketInitialize(&socketConfig);
    if (R_FAILED(rc)) {
        logMsg("socketInitialize failed (%s)", resultToString(rc).c_str());
        if (nifmReady)
            nifmExit();
        return;
    }

    Result timeRc = timeInitialize();
    if (R_SUCCEEDED(timeRc))
        __libnx_init_time();
    else
        logMsg("timeInitialize failed (%s)", resultToString(timeRc).c_str());

    try {
        runExportAndUpload(cfg);
    } catch (const std::exception& exception) {
        logMsg("unhandled exception during export/upload: %s", exception.what());
    } catch (...) {
        logMsg("unhandled unknown exception during export/upload");
    }

    if (R_SUCCEEDED(timeRc))
        timeExit();

    socketExit();
    if (nifmReady)
        nifmExit();
}

int main()
{
    // Let the home menu and system services finish booting before touching
    // pgl/PDM. This also keeps a broken/slow SD card from stalling the boot.
    svcSleepThread(5'000'000'000ULL);

    ensureDataDir();
    logMsg("sysmodule started");

    const Result rc = pglInitialize();
    if (R_FAILED(rc)) {
        logMsg("pglInitialize failed (%s)", resultToString(rc).c_str());
        return 1;
    }

    u64 currentApplicationPid = 0;

    while (true) {
        u64 applicationPid = 0;
        Result pidResult = pglGetApplicationProcessId(&applicationPid);
        if (R_FAILED(pidResult))
            applicationPid = 0;

        if (applicationPid != 0 && applicationPid != currentApplicationPid) {
            currentApplicationPid = applicationPid;
            logMsg("application started (pid=%016llx)", (unsigned long long)currentApplicationPid);
        } else if (applicationPid == 0 && currentApplicationPid != 0) {
            currentApplicationPid = 0;
            logMsg("application exited; waiting for play data");
            svcSleepThread(3'000'000'000ULL);
            uploadCurrentProfiles();
        }

        svcSleepThread(1'000'000'000ULL);
    }

    pglExit();
    return 0;
}

#endif // PHALK_SYSMODULE
