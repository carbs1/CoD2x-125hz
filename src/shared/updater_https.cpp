// http_client.h pulls in mongoose -> winsock2.h, which must be included before windows.h.
#include "http_client.h"
#include "json.h"

#include "updater_https.h"

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <ctype.h>

#include "shared.h"        // WL, FORK_*
#include "version.h"       // APP_VERSION
#include "logger.h"        // logger_add (thread-safe)


// Redirects followed for one check: github.com sends the "latest" download to the asset storage host
#define UPDATE_MAX_REDIRECTS 4


// Only allow one in-flight HTTPS check at a time (update checks can be triggered repeatedly).
static std::atomic<bool> updater_https_inProgress(false);

// Result of the last finished check, handed over to the main thread by updater_https_pollResult().
static std::mutex updater_https_resultMutex;
static bool updater_https_hasResult = false;
static UpdaterHttpsResult updater_https_result;


// Build number from a version string of this fork ("1.4.6.9-125hz.2" -> 2), 0 if it has none
static int updater_https_buildFromVersion(const char* version) {
    const char* p = strstr(version, FORK_VERSION_TAG);
    if (!p)
        return 0;
    p += strlen(FORK_VERSION_TAG);
    if (!isdigit((unsigned char)*p))
        return 0;
    int build = 0;
    for (; *p; p++) {
        if (!isdigit((unsigned char)*p) || build > 100000)
            return 0;
        build = build * 10 + (*p - '0');
    }
    return build;
}

int updater_https_ownBuild() {
    return updater_https_buildFromVersion(APP_VERSION);
}


// Parse update.json into 'out'. The download URL is built from the version, so the manifest can only
// point at a file of a release in this repository.
static bool updater_https_parseManifest(const char* body, UpdaterHttpsResult* out, std::string* error) {
    double num;
    if (!json_get_num(body, "$.build", &num) || num < 1 || num > 100000) {
        *error = "Invalid build number";
        return false;
    }
    const int build = (int)num;

    if (!json_get_num(body, "$.protocol", &num)) {
        *error = "Missing protocol";
        return false;
    }
    const int protocol = (int)num;

    char version[64];
    if (!json_get_str(body, "$.version", version, sizeof(version)) || updater_https_buildFromVersion(version) != build) {
        *error = "Version does not match build number";
        return false;
    }
    for (const char* p = version; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') {
            *error = "Invalid version";
            return false;
        }
    }

    char sha[80];
    if (!json_get_str(body, WL("$.windows.sha256", "$.linux.sha256"), sha, sizeof(sha)) || strlen(sha) != 64) {
        *error = "Invalid SHA-256";
        return false;
    }
    for (char* p = sha; *p; p++) {
        if (!isxdigit((unsigned char)*p)) {
            *error = "Invalid SHA-256";
            return false;
        }
        *p = (char)tolower((unsigned char)*p);
    }

    const int ownBuild = updater_https_ownBuild();
    if (build <= ownBuild)
        out->available = 0;
    else if (protocol != FORK_PROTOCOL)
        out->available = 2; // servers of the new build reject this one, so the update is required
    else
        out->available = 1;

    out->version = version;
    out->sha256 = sha;
    out->url = std::string(FORK_DOWNLOAD_URL "v") + version + WL("/mss32.dll", "/libCoD2x.so");
    return true;
}

static void updater_https_setResult(const UpdaterHttpsResult& result) {
    std::lock_guard<std::mutex> lock(updater_https_resultMutex);
    updater_https_result = result;
    updater_https_hasResult = true;
}

// Case-insensitive header lookup
static std::string updater_https_header(const HttpClient::Response& res, const char* name) {
    for (const auto& h : res.headers) {
        if (strcasecmp(h.first.c_str(), name) == 0)
            return h.second;
    }
    return "";
}


bool updater_https_checkForUpdate() {
    if (updater_https_ownBuild() == 0) {
        logger_add("[UPDATER-HTTPS] Development build %s, update check skipped", APP_VERSION);
        return false;
    }

    // Skip if a previous HTTPS check is still running.
    bool expected = false;
    if (!updater_https_inProgress.compare_exchange_strong(expected, true)) {
        return false;
    }

    // Run the whole round-trip on a background thread so the caller is never blocked.
    std::thread([]() {

        UpdaterHttpsResult result;
        result.error = "No response";

        std::string url = FORK_UPDATE_URL;
        for (int hop = 0; hop <= UPDATE_MAX_REDIRECTS; hop++) {
            HttpClient::Response response;
            bool done = false;

            HttpClient client;
            client.get(
                url.c_str(),
                "User-Agent: CoD2x/" APP_VERSION,
                [&](const HttpClient::Response& res) {
                    response = res;
                    done = true;
                },
                [&result](const std::string& err) {
                    result.error = err;
                    logger_add("[UPDATER-HTTPS] Request failed: %s", err.c_str());
                },
                5000);

            // Drive the client until the request completes (connection closed) or times out.
            client.poll_max(7000);
            if (!done)
                break;

            const int status = response.status;
            if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                url = updater_https_header(response, "Location");
                if (strncmp(url.c_str(), "https://", 8) != 0) {
                    result.error = "Invalid redirect";
                    break;
                }
                continue;
            }

            if (status < 200 || status >= 300) {
                result.error = "HTTP " + std::to_string(status);
                logger_add("[UPDATER-HTTPS] Request rejected (HTTP %d)", status);
                break;
            }

            UpdaterHttpsResult parsed;
            std::string error;
            if (!updater_https_parseManifest(response.body.c_str(), &parsed, &error)) {
                result.error = "Invalid update.json: " + error;
                logger_add("[UPDATER-HTTPS] %s", result.error.c_str());
                break;
            }

            parsed.ok = true;
            result = parsed;
            logger_add("[UPDATER-HTTPS] available=%d version='%s' url='%s'",
                parsed.available, parsed.version.c_str(), parsed.url.c_str());
            break;
        }

        updater_https_setResult(result);
        updater_https_inProgress = false;
    }).detach();

    return true;
}


bool updater_https_pollResult(UpdaterHttpsResult* out) {
    std::lock_guard<std::mutex> lock(updater_https_resultMutex);
    if (!updater_https_hasResult)
        return false;
    *out = updater_https_result;
    updater_https_hasResult = false;
    return true;
}
