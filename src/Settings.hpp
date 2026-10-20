#pragma once
// Settings (~/.local/state/shaders/config.json, one section per shader) and the
// Material palette (~/.local/state/theme/palette.json). Both are polled by mtime.

#include <nlohmann/json.hpp>
#include <array>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace hs {
    struct SColor {
        float r = 0, g = 0, b = 0, a = 1;
    };

    std::optional<SColor> parseHexColor(const std::string& hex);

    class CSettings {
      public:
        CSettings();

        // stat the files and reload the ones that changed; returns true if anything changed
        bool                  poll();

        // section for a shader (null json if absent)
        const nlohmann::json& section(const std::string& shader) const;

        // palette role (e.g. "primary"), or fallback
        SColor                color(const std::string& role, SColor fallback) const;

        // bumps every time config or palette changes
        uint64_t generation() const {
            return m_generation;
        }

        std::string configPath, legacyConfigPath, palettePath;

      private:
        struct SWatch {
            int64_t mtimeNs = -1;
            int64_t size    = -1;
        };

        bool                                    changed(const std::string& path, SWatch& w);
        void                                    loadConfig();
        void                                    loadPalette();

        SWatch                                  m_cfgWatch, m_legacyWatch, m_palWatch;
        nlohmann::json                          m_config = nlohmann::json::object();
        std::unordered_map<std::string, SColor> m_palette;
        uint64_t                                m_generation = 1;
    };

    CSettings& settings();

    // helpers to read numbers out of a json section with defaults
    float       jnum(const nlohmann::json& j, const char* key, float def);
    std::string jstr(const nlohmann::json& j, const char* key, const std::string& def);

    std::string homeDir();
    std::string xdgDir(const char* env, const char* fallbackUnderHome);
}
