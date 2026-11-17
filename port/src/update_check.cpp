#include "update_check.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif

#ifndef COI_VERSION
#define COI_VERSION "0.0.0"
#endif

namespace {

// The newest release first, prereleases included (/releases/latest leaves those out, and every
// release so far is one). No user data is sent: GitHub's API only asks for a User-Agent.
constexpr const char* kApiHost = "api.github.com";
constexpr const char* kApiPath = "/repos/saphiron86/castle-of-illusion-port/releases?per_page=1";
constexpr const char* kUserAgent = "coi-port-update-check";

std::mutex g_lock;
std::string g_newer;                                // the newer tag, once found
std::atomic<bool> g_started{false};

// The first "tag_name" in GitHub's JSON reply.
std::string tag_from(const std::string& json)
{
    const size_t key = json.find("\"tag_name\"");
    if (key == std::string::npos)
        return "";
    const size_t open = json.find('"', json.find(':', key) + 1);
    const size_t close = open == std::string::npos ? open : json.find('"', open + 1);
    return close == std::string::npos ? "" : json.substr(open + 1, close - open - 1);
}

// "v0.5.2-beta" -> {0, 5, 2} and whether it has a suffix (a prerelease of that number).
struct Version {
    int part[3] = {};
    bool suffix = false;
    bool valid = false;
};
Version parse(std::string s)
{
    Version v;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
        s.erase(0, 1);
    const char* p = s.c_str();
    for (int i = 0; i < 3; i++) {
        char* end;
        const long n = std::strtol(p, &end, 10);
        if (end == p)
            return v;
        v.part[i] = (int)n;
        p = end;
        if (i < 2) {
            if (*p != '.')
                return v;
            p++;
        }
    }
    v.suffix = *p == '-';
    v.valid = true;
    return v;
}

bool newer(const Version& a, const Version& b)     // a newer than b
{
    for (int i = 0; i < 3; i++)
        if (a.part[i] != b.part[i])
            return a.part[i] > b.part[i];
    return !a.suffix && b.suffix;                   // 0.6.0 after 0.6.0-beta
}

void found(const std::string& tag)
{
    const Version latest = parse(tag), mine = parse(COI_VERSION);
    if (!latest.valid || !mine.valid || !newer(latest, mine))
        return;
    std::lock_guard<std::mutex> hold(g_lock);
    g_newer = tag;
}

#if defined(_WIN32)
std::string fetch()
{
    std::string body;
    wchar_t agent[64], host[64];
    std::swprintf(agent, 64, L"%hs", kUserAgent);
    std::swprintf(host, 64, L"%hs", kApiHost);
    wchar_t path[128];
    std::swprintf(path, 128, L"%hs", kApiPath);
    HINTERNET session = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return body;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available;) {
                std::vector<char> chunk(available);
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || !read)
                    break;
                body.append(chunk.data(), read);
                if (body.size() > (1 << 20))
                    break;
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}
#elif !defined(__ANDROID__)
// Linux: the system's curl program (SteamOS, Bazzite and the usual desktops have it), so the
// port needs no TLS library of its own. Without it, nothing is checked.
std::string fetch()
{
    std::string body;
    char command[512];
    std::snprintf(command, sizeof(command),
                  "curl -fsSL --max-time 10 -A '%s' -H 'Accept: application/vnd.github+json' 'https://%s%s' 2>/dev/null",
                  kUserAgent, kApiHost, kApiPath);
    if (FILE* pipe = popen(command, "r")) {
        char buffer[4096];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0 && body.size() < (1 << 20);)
            body.append(buffer, n);
        pclose(pipe);
    }
    return body;
}
#endif

}  // namespace

const char* app_version() { return COI_VERSION; }

void update_check_start()
{
    if (g_started.exchange(true))
        return;
#ifdef __ANDROID__
    // The activity fetches on its own thread (CoiActivity.startUpdateCheck); update_available
    // reads its answer.
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity)
        return;
    jclass cls = env->GetObjectClass(activity);
    if (jmethodID start = env->GetStaticMethodID(cls, "startUpdateCheck", "()V"))
        env->CallStaticVoidMethod(cls, start);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
#else
    std::thread([] { found(tag_from(fetch())); }).detach();
#endif
}

std::string update_available()
{
#ifdef __ANDROID__
    // Polled until the activity has an answer (a few times a second is plenty).
    static bool answered = false;
    static Uint64 last = 0;
    if (g_started && !answered && SDL_GetTicks() - last > 500) {
        last = SDL_GetTicks();
        auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
        auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
        if (env && activity) {
            jclass cls = env->GetObjectClass(activity);
            jmethodID get = env->GetStaticMethodID(cls, "updateCheckResult", "()Ljava/lang/String;");
            if (auto result = get ? (jstring)env->CallStaticObjectMethod(cls, get) : nullptr) {
                const char* json = env->GetStringUTFChars(result, nullptr);
                found(tag_from(json));
                env->ReleaseStringUTFChars(result, json);
                env->DeleteLocalRef(result);
                answered = true;
            }
            env->DeleteLocalRef(cls);
            env->DeleteLocalRef(activity);
        }
    }
#endif
    std::lock_guard<std::mutex> hold(g_lock);
    return g_newer;
}
