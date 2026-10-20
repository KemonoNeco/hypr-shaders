#include "Settings.hpp"
#include "Log.hpp"

#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <cstdlib>

using namespace hs;
using json = nlohmann::json;

std::string hs::homeDir() {
    const char* h = getenv("HOME");
    return h && *h ? h : "/tmp";
}

std::string hs::xdgDir(const char* env, const char* fallbackUnderHome) {
    const char* v = getenv(env);
    if (v && *v == '/')
        return v;
    return homeDir() + "/" + fallbackUnderHome;
}

CSettings& hs::settings() {
    static CSettings s;
    return s;
}

CSettings::CSettings() {
    const auto state = xdgDir("XDG_STATE_HOME", ".local/state");
    configPath       = state + "/shaders/config.json";
    legacyConfigPath = state + "/ascii/config.json";
    palettePath      = state + "/theme/palette.json";
}

std::optional<SColor> hs::parseHexColor(const std::string& hexIn) {
    std::string hex = hexIn;
    if (hex.starts_with("#"))
        hex = hex.substr(1);
    if (hex.size() != 6 && hex.size() != 8)
        return std::nullopt;
    for (char c : hex)
        if (!isxdigit((unsigned char)c))
            return std::nullopt;
    auto byte = [&](int i) { return std::stoi(hex.substr(i * 2, 2), nullptr, 16) / 255.F; };
    SColor col{byte(0), byte(1), byte(2), hex.size() == 8 ? byte(3) : 1.F};
    return col;
}

static bool readJson(const std::string& path, json& out) {
    std::ifstream f(path);
    if (!f.good())
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        out = json::parse(ss.str());
        return true;
    } catch (std::exception& e) {
        // the file is written live by the bar; a half-written file just means "try again later"
        log(WARN, "failed to parse {}: {}", path, e.what());
        return false;
    }
}

bool CSettings::changed(const std::string& path, SWatch& w) {
    struct stat st;
    int64_t     mt = -2, sz = -2;
    if (stat(path.c_str(), &st) == 0) {
        mt = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
        sz = st.st_size;
    }
    if (mt == w.mtimeNs && sz == w.size)
        return false;
    w.mtimeNs = mt;
    w.size    = sz;
    return true;
}

void CSettings::loadConfig() {
    json j;
    if (readJson(configPath, j) && j.is_object()) {
        m_config = j;
        return;
    }

    struct stat st;
    if (stat(configPath.c_str(), &st) == 0)
        return; // exists but unparsable (mid-write): keep the old values

    // fall back to the pre-"shaders" location: {"live": {...ascii settings...}}
    json legacy;
    if (readJson(legacyConfigPath, legacy) && legacy.is_object() && legacy.contains("live") && legacy["live"].is_object()) {
        m_config          = json::object();
        m_config["ascii"] = legacy["live"];
    }
}

void CSettings::loadPalette() {
    json j;
    if (!readJson(palettePath, j) || !j.is_object() || !j.contains("colors") || !j["colors"].is_object())
        return;

    m_palette.clear();
    for (auto& [k, v] : j["colors"].items()) {
        if (!v.is_string())
            continue;
        if (auto c = parseHexColor(v.get<std::string>()); c)
            m_palette[k] = *c;
    }
}

bool CSettings::poll() {
    bool any = false;
    // evaluate all three so each watch state stays current
    const bool cfg    = changed(configPath, m_cfgWatch);
    const bool legacy = changed(legacyConfigPath, m_legacyWatch);
    if (cfg || legacy) {
        loadConfig();
        any = true;
    }
    if (changed(palettePath, m_palWatch)) {
        loadPalette();
        any = true;
    }
    if (any)
        m_generation++;
    return any;
}

const json& CSettings::section(const std::string& shader) const {
    static const json NUL;
    auto              it = m_config.find(shader);
    if (it == m_config.end() || !it->is_object())
        return NUL;
    return *it;
}

SColor CSettings::color(const std::string& role, SColor fallback) const {
    if (auto c = parseHexColor(role); c)
        return *c; // allow literal colours where a role is expected
    auto it = m_palette.find(role);
    return it == m_palette.end() ? fallback : it->second;
}

float hs::jnum(const json& j, const char* key, float def) {
    if (!j.is_object())
        return def;
    auto it = j.find(key);
    if (it == j.end())
        return def;
    if (it->is_number())
        return it->get<float>();
    if (it->is_boolean())
        return it->get<bool>() ? 1.F : 0.F;
    return def;
}

std::string hs::jstr(const json& j, const char* key, const std::string& def) {
    if (!j.is_object())
        return def;
    auto it = j.find(key);
    if (it == j.end())
        return def;
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_number_integer())
        return std::to_string(it->get<long long>());
    return def;
}
