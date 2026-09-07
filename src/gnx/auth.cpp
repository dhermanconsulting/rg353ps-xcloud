#include "auth.hpp"

#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "../../third_party/nlohmann/json.hpp"

using nlohmann::json;

namespace gnx {

namespace {

// Public client id used by open xCloud clients for the device-code flow.
constexpr const char* kClientId = "1f907974-e22b-4810-a9de-d9647380c97e";
constexpr const char* kScope = "xboxlive.signin openid profile offline_access";
constexpr const char* kDeviceCodeUrl =
    "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
constexpr const char* kTokenUrl =
    "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";

json parse_json(const HttpResponse& response, const char* label) {
    json parsed = json::parse(response.body, nullptr, false);
    if (parsed.is_discarded())
        throw std::runtime_error(std::string(label) + ": invalid JSON: " +
                                 response.body.substr(0, 300));
    return parsed;
}

json expect_ok(const HttpResponse& response, const char* label) {
    if (!response.ok())
        throw std::runtime_error(std::string(label) + " failed with HTTP " +
                                 std::to_string(response.status) + ": " +
                                 response.body.substr(0, 300));
    return parse_json(response, label);
}

const std::vector<std::string> kFormHeaders = {
    "Content-Type: application/x-www-form-urlencoded"};
const std::vector<std::string> kXblHeaders = {
    "Content-Type: application/json", "x-xbl-contract-version: 1"};

}  // namespace

XboxAuth::XboxAuth(std::string token_store_path)
    : store_path_(std::move(token_store_path)) {
    load_refresh_token();
}

void XboxAuth::logout() {
    refresh_token_.clear();
    std::remove(store_path_.c_str());
}

void XboxAuth::set_token_store(std::string path) {
    store_path_ = std::move(path);
    refresh_token_.clear();
    load_refresh_token();
}

// The refresh token is a long-lived credential to the user's Microsoft
// account, so this differs from upstream in two ways that matter:
//
//   - 0600, not whatever the umask gives. The handheld runs everything as
//     root and has no other user, but the file is also what gets copied off
//     the device by anyone debugging with scripts/pull.sh, and it should not
//     be world-readable wherever it lands.
//   - Write a temporary file and rename over the target, rather than
//     truncating in place. A truncating write that then fails -- a full
//     /userdata is the realistic case -- leaves an empty file, which reads
//     back as "not signed in" and silently costs the user their session.
//     rename(2) within a directory is atomic, so the old token survives any
//     failure before it.
void XboxAuth::save_refresh_token(const std::string& token) {
    refresh_token_ = token;
    if (store_path_.empty())
        return;

    const std::string tmp = store_path_ + ".new";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return;
        out << json{{"refresh_token", token}}.dump(2);
        out.flush();
        if (!out) {           // write failed; leave the existing token alone
            std::remove(tmp.c_str());
            return;
        }
    }
    // Before the rename, so the token is never briefly readable at 0644.
    //
    // Not fatal if it fails. The store can legitimately sit on a filesystem
    // with no permission bits to set -- exFAT, which is what the removable
    // card on this device is formatted as -- and there chmod returns EPERM.
    // Losing the token over that would mean signing in again on every single
    // launch, which is far worse than a file that cannot be mode-restricted
    // on a single-user handheld. Say so once rather than silently.
    if (::chmod(tmp.c_str(), S_IRUSR | S_IWUSR) != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "xcloud: cannot restrict permissions on %s (%s); "
                         "the sign-in token is stored unprotected\n",
                         store_path_.c_str(), std::strerror(errno));
        }
    }
    if (std::rename(tmp.c_str(), store_path_.c_str()) != 0)
        std::remove(tmp.c_str());
}

// See the declaration for why this exists. Checks the invariants that decide
// whether a signed-in user stays signed in:
//
//   1. a saved value reads back through the normal load path;
//   2. a second save replaces the first rather than appending or failing;
//   3. nothing is left behind in the directory (the .new temporary);
//   4. permissions are 0600 where the filesystem has them -- reported, not
//      required, because the ROM card is exFAT and has none.
bool XboxAuth::selftest_store(const std::string& dir, std::string* detail) {
    const std::string path = dir + "/.selftest-token.json";
    const std::string tmp = path + ".new";
    const std::string first = "selftest-first-value";
    const std::string second = "selftest-second-value-longer";
    std::string note;
    bool ok = true;

    std::remove(path.c_str());
    std::remove(tmp.c_str());

    {
        XboxAuth a(path);
        a.save_refresh_token(first);
    }
    {
        XboxAuth b(path);      // constructor loads
        if (!b.has_saved_login() || b.refresh_token_ != first) {
            note += "save/load round trip failed; ";
            ok = false;
        }
    }

    // Overwrite. The rename path is what makes this safe, and a partial
    // implementation would show up here as the old value surviving.
    {
        XboxAuth c(path);
        c.save_refresh_token(second);
    }
    {
        XboxAuth d(path);
        if (d.refresh_token_ != second) {
            note += "second save did not replace the first; ";
            ok = false;
        }
    }

    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        note += "token file missing after save; ";
        ok = false;
    } else if (st.st_size <= 0) {
        note += "token file is empty; ";
        ok = false;
    } else if ((st.st_mode & 077) != 0) {
        // Not a failure: exFAT and vfat cannot represent this at all.
        note += "permissions not restricted (filesystem has none?); ";
    }

    if (::stat(tmp.c_str(), &st) == 0) {
        note += "left a .new temporary behind; ";
        ok = false;
        std::remove(tmp.c_str());
    }

    std::remove(path.c_str());
    if (detail)
        *detail = note.empty() ? "save, reload, replace, 0600, no litter" : note;
    return ok;
}

void XboxAuth::load_refresh_token() {
    std::ifstream in(store_path_);
    if (!in) return;
    json data = json::parse(in, nullptr, false);
    if (!data.is_discarded())
        refresh_token_ = data.value("refresh_token", "");
}

DeviceCode XboxAuth::request_device_code() {
    std::string body = std::string("client_id=") + kClientId +
                       "&scope=" + Http::urlencode(kScope);
    json response =
        expect_ok(http_.post(kDeviceCodeUrl, body, kFormHeaders), "device code");

    DeviceCode code;
    code.user_code = response.at("user_code");
    code.device_code = response.at("device_code");
    code.verification_uri = response.at("verification_uri");
    code.message = response.value("message", "");
    code.interval_secs = response.value("interval", 5);
    code.expires_in_secs = response.value("expires_in", 900);
    return code;
}

PollResult XboxAuth::poll_device_code(const DeviceCode& code) {
    std::string body =
        std::string(
            "grant_type=urn:ietf:params:oauth:grant-type:device_code") +
        "&client_id=" + kClientId + "&device_code=" + code.device_code;
    HttpResponse response = http_.post(kTokenUrl, body, kFormHeaders);

    if (!response.ok()) {
        json error = parse_json(response, "device code poll");
        std::string kind = error.value("error", "");
        if (kind == "authorization_pending" || kind == "slow_down")
            return PollResult::Pending;
        if (kind == "expired_token" || kind == "bad_verification_code")
            return PollResult::Expired;
        throw std::runtime_error("device code poll rejected: " +
                                 error.value("error_description", kind));
    }

    json token = parse_json(response, "device code poll");
    save_refresh_token(token.at("refresh_token"));
    return PollResult::Authorized;
}

std::string XboxAuth::refresh_user_token() {
    if (refresh_token_.empty())
        throw std::runtime_error("not signed in — run login first");

    std::string body = std::string("client_id=") + kClientId +
                       "&grant_type=refresh_token&refresh_token=" +
                       refresh_token_ + "&scope=" + Http::urlencode(kScope);
    HttpResponse response = http_.post(kTokenUrl, body, kFormHeaders);
    if (!response.ok()) {
        json error = parse_json(response, "token refresh");
        if (error.value("error", "") == "invalid_grant") {
            logout();
            throw std::runtime_error(
                "saved login expired; please sign in again");
        }
        throw std::runtime_error("token refresh failed: " +
                                 response.body.substr(0, 300));
    }

    json token = parse_json(response, "token refresh");
    save_refresh_token(token.at("refresh_token"));
    return token.at("access_token");
}

std::string XboxAuth::xsts_user_authenticate(const std::string& access_token) {
    json body = {
        {"Properties",
         {{"AuthMethod", "RPS"},
          {"RpsTicket", "d=" + access_token},
          {"SiteName", "user.auth.xboxlive.com"}}},
        {"RelyingParty", "http://auth.xboxlive.com"},
        {"TokenType", "JWT"},
    };
    json response = expect_ok(
        http_.post("https://user.auth.xboxlive.com/user/authenticate",
                   body.dump(), kXblHeaders),
        "XSTS user authenticate");
    return response.at("Token");
}

std::pair<std::string, std::string> XboxAuth::xsts_authorize(
    const std::string& user_token, const std::string& relying_party) {
    json body = {
        {"Properties",
         {{"SandboxId", "RETAIL"}, {"UserTokens", {user_token}}}},
        {"RelyingParty", relying_party},
        {"TokenType", "JWT"},
    };
    json response = expect_ok(
        http_.post("https://xsts.auth.xboxlive.com/xsts/authorize",
                   body.dump(), kXblHeaders),
        "XSTS authorize");

    std::string uhs;
    if (response.contains("DisplayClaims")) {
        const json& xui = response["DisplayClaims"].value("xui", json::array());
        if (!xui.empty()) uhs = xui.front().value("uhs", "");
    }
    return {response.at("Token"), uhs};
}

EndpointCredentials XboxAuth::streaming_login(const std::string& gssv_token,
                                              const std::string& offering) {
    json body = {{"token", gssv_token}, {"offeringId", offering}};
    json response = expect_ok(
        http_.post("https://" + offering +
                       ".gssv-play-prod.xboxlive.com/v2/login/user",
                   body.dump(),
                   {"Content-Type: application/json",
                    "x-gssv-client: XboxComBrowser"}),
        ("streaming login " + offering).c_str());

    EndpointCredentials credentials;
    credentials.token = response.at("gsToken");
    for (const json& region :
         response.at("offeringSettings").value("regions", json::array())) {
        if (region.value("isDefault", false)) {
            credentials.host = region.value("baseUri", "");
            break;
        }
    }
    if (credentials.host.empty())
        throw std::runtime_error("streaming login " + offering +
                                 ": no default region");
    return credentials;
}

StreamingCredentials XboxAuth::fetch_streaming_credentials() {
    std::string access_token = refresh_user_token();
    std::string user_token = xsts_user_authenticate(access_token);
    auto [gssv_token, uhs] =
        xsts_authorize(user_token, "http://gssv.xboxlive.com/");
    (void)uhs;

    StreamingCredentials credentials;
    try {
        credentials.home = streaming_login(gssv_token, "xhome");
    } catch (const std::exception& error) {
        // No console attached to the account — cloud-only is fine. Keep the
        // reason: it is the only explanation available if the user then tries
        // to stream from a console anyway.
        credentials.home_error = error.what();
    }
    credentials.cloud = streaming_login(gssv_token, "xgpuweb");
    try {
        credentials.cloud_f2p = streaming_login(gssv_token, "xgpuwebf2p");
    } catch (const std::exception&) {
        credentials.cloud_f2p = std::nullopt;
    }
    return credentials;
}

std::string XboxAuth::fetch_passport_token() {
    refresh_user_token();
    if (refresh_token_.empty())
        throw std::runtime_error("not signed in");
    std::string body =
        std::string("client_id=") + kClientId +
        "&scope=service::http://Passport.NET/purpose::"
        "PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN"
        "&grant_type=refresh_token&refresh_token=" + refresh_token_;
    json response =
        expect_ok(http_.post("https://login.live.com/oauth20_token.srf", body,
                             kFormHeaders),
                  "passport token");
    return response.at("access_token");
}

XboxProfile XboxAuth::fetch_profile() {
    std::string access_token = refresh_user_token();
    std::string user_token = xsts_user_authenticate(access_token);
    auto [xbl_token, uhs] = xsts_authorize(user_token, "http://xboxlive.com");

    json response = expect_ok(
        http_.get("https://profile.xboxlive.com/users/me/profile/settings"
                  "?settings=GameDisplayPicRaw,Gamertag,Gamerscore",
                  {"x-xbl-contract-version: 3",
                   "Authorization: XBL3.0 x=" + uhs + ";" + xbl_token}),
        "profile");

    XboxProfile profile;
    for (const json& user :
         response.value("profileUsers", json::array())) {
        for (const json& setting : user.value("settings", json::array())) {
            std::string id = setting.value("id", "");
            std::string value = setting.value("value", "");
            if (id == "Gamertag") profile.gamertag = value;
            else if (id == "Gamerscore") profile.gamerscore = value;
            else if (id == "GameDisplayPicRaw") profile.avatar_url = value;
        }
    }
    return profile;
}

}  // namespace gnx
