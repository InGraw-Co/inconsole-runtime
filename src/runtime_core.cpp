#include "runtime.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>

namespace inconsole {

namespace {

std::string now_timestamp() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

bool extract_string(const std::string &json, const std::string &key, std::string *out);
bool read_file_int(const std::string &path, int *out);

std::string shell_single_quote(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 8);
    out.push_back('\'');
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\'')
            out += "'\\''";
        else
            out.push_back(value[i]);
    }
    out.push_back('\'');
    return out;
}

std::string collapse_ws(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    bool last_space = false;
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        if (std::isspace(ch)) {
            if (!out.empty() && !last_space) out.push_back(' ');
            last_space = true;
        } else {
            out.push_back(static_cast<char>(ch));
            last_space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string run_command_capture(const std::string &cmd, int *status) {
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        if (status) *status = -1;
        return std::string();
    }

    std::string output;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) output += buf;

    int rc = pclose(pipe);
    if (status) *status = rc;
    return output;
}

bool ensure_pwm_channel_exported(const std::string &base, int channel, Logger *logger) {
    const std::string pwm_dir = base + "/pwm" + std::to_string(channel);
    struct stat st;
    if (stat(pwm_dir.c_str(), &st) == 0) return true;

    const std::string export_path = base + "/export";
    std::ostringstream ss;
    ss << channel << "\n";
    if (!write_text_file(export_path, ss.str())) {
        if (logger) logger->warn("Unable to export PWM channel " + std::to_string(channel) + " at " + base);
        return false;
    }

    for (int attempt = 0; attempt < 20; ++attempt) {
        if (stat(pwm_dir.c_str(), &st) == 0) return true;
        usleep(10000);
    }
    if (logger) logger->warn("PWM channel export timed out for " + pwm_dir);
    return false;
}

bool write_pwm_backlight_percent(int percent, Logger *logger) {
    DIR *dir = opendir("/sys/class/pwm");
    if (!dir) return false;

    bool applied = false;
    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "pwmchip", 7) != 0) continue;
        const std::string base = std::string("/sys/class/pwm/") + ent->d_name;
        int npwm = 0;
        if (!read_file_int(base + "/npwm", &npwm) || npwm <= 7) continue;
        if (!ensure_pwm_channel_exported(base, 7, logger)) continue;

        const std::string pwm_dir = base + "/pwm7";
        const std::string enable_path = pwm_dir + "/enable";
        const std::string period_path = pwm_dir + "/period";
        const std::string duty_path = pwm_dir + "/duty_cycle";
        const std::string polarity_path = pwm_dir + "/polarity";
        const int clamped = std::max(0, std::min(100, percent));
        int period_ns = 1000000;
        if (!read_file_int(period_path, &period_ns) || period_ns <= 0) period_ns = 1000000;
        const int duty_ns = (period_ns * clamped) / 100;

        write_text_file(enable_path, "0\n");
        if (access(polarity_path.c_str(), F_OK) == 0) write_text_file(polarity_path, "normal\n");

        std::ostringstream period_ss;
        period_ss << period_ns << "\n";
        std::ostringstream duty_ss;
        duty_ss << duty_ns << "\n";
        if (!write_text_file(period_path, period_ss.str())) continue;
        if (!write_text_file(duty_path, duty_ss.str())) continue;
        if (!write_text_file(enable_path, "1\n")) continue;

        applied = true;
        if (logger) {
            logger->info("Applied PWM7 backlight brightness " + std::to_string(clamped) + "% via " + ent->d_name);
        }
        break;
    }

    closedir(dir);
    return applied;
}

std::string read_first_kv_value(const std::string &path, const char *key_prefix) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return std::string();
    char line[512];
    std::string out;
    const size_t key_len = strlen(key_prefix);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key_prefix, key_len) == 0) {
            const char *p = strchr(line, ':');
            if (p) {
                ++p;
                while (*p == ' ' || *p == '\t') ++p;
                out = p;
                while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            }
            break;
        }
    }
    fclose(f);
    return out;
}

long read_mem_total_kb() {
    std::string s = read_first_kv_value("/proc/meminfo", "MemTotal");
    if (s.empty()) return 0;
    long kb = strtol(s.c_str(), nullptr, 10);
    return kb > 0 ? kb : 0;
}

long free_disk_mib(const std::string &path) {
    struct statvfs st;
    if (statvfs(path.c_str(), &st) != 0) return 0;
    unsigned long long bytes = static_cast<unsigned long long>(st.f_bavail) * static_cast<unsigned long long>(st.f_frsize);
    return static_cast<long>(bytes / (1024ULL * 1024ULL));
}

struct LangPack {
    std::string code;
    std::string name;
    std::map<std::string, std::string> strings;
};

struct ThemePack {
    Theme theme;
    bool builtin;

    ThemePack() : theme(), builtin(false) {}
};

std::string language_dir_path() {
    const char *env = getenv("INCONSOLE_DATA_ROOT");
    const std::string root = (env && env[0] != '\0') ? std::string(env) : std::string("/userdata");
    return root + "/system/languages";
}

std::string theme_dir_path() {
    const char *env = getenv("INCONSOLE_DATA_ROOT");
    const std::string root = (env && env[0] != '\0') ? std::string(env) : std::string("/userdata");
    return root + "/themes";
}

bool extract_object(const std::string &json, const std::string &key, std::string *out) {
    std::regex re("\"" + key + "\"\\s*:\\s*\\{([\\s\\S]*?)\\}");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return false;
    if (m.size() < 2) return false;
    *out = m[1].str();
    return true;
}

std::map<std::string, LangPack> &lang_cache() {
    static std::map<std::string, LangPack> cache;
    return cache;
}

std::vector<std::string> &lang_order_cache() {
    static std::vector<std::string> order;
    return order;
}

bool &lang_loaded_flag() {
    static bool loaded = false;
    return loaded;
}

std::map<std::string, ThemePack> &theme_cache() {
    static std::map<std::string, ThemePack> cache;
    return cache;
}

std::vector<std::string> &theme_order_cache() {
    static std::vector<std::string> order;
    return order;
}

bool &theme_loaded_flag() {
    static bool loaded = false;
    return loaded;
}

bool parse_theme_motion(const std::string &raw) {
    std::string norm = raw;
    for (size_t i = 0; i < norm.size(); ++i) {
        norm[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(norm[i])));
    }
    return norm == "flow" || norm == "wave" || norm == "animated" || norm == "1" || norm == "true";
}

void register_theme_pack(const ThemePack &pack) {
    std::map<std::string, ThemePack> &cache = theme_cache();
    std::vector<std::string> &order = theme_order_cache();
    const std::string id = pack.theme.id;
    if (id.empty()) return;
    if (cache.count(id) == 0) order.push_back(id);
    cache[id] = pack;
}

void ensure_languages_loaded() {
    if (lang_loaded_flag()) return;
    lang_loaded_flag() = true;

    std::map<std::string, LangPack> &cache = lang_cache();
    std::vector<std::string> &order = lang_order_cache();
    cache.clear();
    order.clear();

    const std::string lang_dir = language_dir_path();
    DIR *dir = opendir(lang_dir.c_str());
    if (dir) {
        struct dirent *ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string fn = ent->d_name;
            if (fn.size() < 6 || fn.substr(fn.size() - 5) != ".json") continue;
            std::string path = lang_dir + "/" + fn;
            std::string json = read_text_file(path);
            if (json.empty()) continue;

            LangPack p;
            if (!extract_string(json, "code", &p.code) || p.code.empty()) {
                p.code = fn.substr(0, fn.size() - 5);
            }
            for (size_t i = 0; i < p.code.size(); ++i) {
                p.code[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(p.code[i])));
            }
            if (!extract_string(json, "name", &p.name) || p.name.empty()) p.name = p.code;

            std::string body;
            if (extract_object(json, "strings", &body)) {
                std::regex pair_re("\"([^\"]+)\"\\s*:\\s*\"([^\"]*)\"");
                std::smatch m;
                std::string::const_iterator it = body.begin();
                while (std::regex_search(it, body.cend(), m, pair_re)) {
                    if (m.size() >= 3) p.strings[m[1].str()] = m[2].str();
                    it = m.suffix().first;
                }
            }
            cache[p.code] = p;
            order.push_back(p.code);
        }
        closedir(dir);
    }

    if (cache.count("pl") == 0) {
        LangPack pl;
        pl.code = "pl";
        pl.name = "Polski";
        cache["pl"] = pl;
        order.push_back("pl");
    }
    if (cache.count("en") == 0) {
        LangPack en;
        en.code = "en";
        en.name = "English";
        cache["en"] = en;
        order.push_back("en");
    }
}

bool extract_string(const std::string &json, const std::string &key, std::string *out) {
    std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return false;
    if (m.size() < 2) return false;
    *out = m[1].str();
    return true;
}

bool extract_int(const std::string &json, const std::string &key, int *out) {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?[0-9]+)");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return false;
    if (m.size() < 2) return false;
    *out = atoi(m[1].str().c_str());
    return true;
}

bool extract_bool(const std::string &json, const std::string &key, bool *out) {
    std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return false;
    if (m.size() < 2) return false;
    *out = m[1].str() == "true";
    return true;
}

bool parse_hex_byte(const std::string &s, size_t off, uint8_t *out) {
    if (!out || off + 2 > s.size()) return false;
    const std::string part = s.substr(off, 2);
    char *end = nullptr;
    long value = strtol(part.c_str(), &end, 16);
    if (!end || *end != '\0' || value < 0 || value > 255) return false;
    *out = static_cast<uint8_t>(value);
    return true;
}

bool parse_color_value(const std::string &raw, Color *out) {
    if (!out) return false;
    std::string s = raw;
    s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }), s.end());
    if (s.empty()) return false;
    if (s[0] == '#') s.erase(s.begin());
    if (s.size() == 6) {
        return parse_hex_byte(s, 0, &out->r) && parse_hex_byte(s, 2, &out->g) && parse_hex_byte(s, 4, &out->b);
    }
    std::regex rgb_re("\\[([0-9]{1,3}),([0-9]{1,3}),([0-9]{1,3})\\]");
    std::smatch m;
    if (!std::regex_match(s, m, rgb_re) || m.size() < 4) return false;
    out->r = static_cast<uint8_t>(std::max(0, std::min(255, atoi(m[1].str().c_str()))));
    out->g = static_cast<uint8_t>(std::max(0, std::min(255, atoi(m[2].str().c_str()))));
    out->b = static_cast<uint8_t>(std::max(0, std::min(255, atoi(m[3].str().c_str()))));
    return true;
}

bool extract_color(const std::string &json, const std::string &key, Color *out) {
    std::string value;
    if (!extract_string(json, key, &value)) return false;
    return parse_color_value(value, out);
}

std::vector<std::string> extract_string_array(const std::string &json, const std::string &key) {
    std::vector<std::string> out;
    std::regex re("\"" + key + "\"\\s*:\\s*\\[(.*?)\\]");
    std::smatch m;
    if (!std::regex_search(json, m, re)) return out;
    if (m.size() < 2) return out;

    std::string body = m[1].str();
    std::regex item_re("\"([^\"]*)\"");
    auto begin = std::sregex_iterator(body.begin(), body.end(), item_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        if ((*it).size() >= 2) out.push_back((*it)[1].str());
    }
    return out;
}

bool read_file_int(const std::string &path, int *out) {
    std::string text = read_text_file(path);
    if (text.empty()) return false;

    const char *begin = text.c_str();
    while (*begin == ' ' || *begin == '\t' || *begin == '\n' || *begin == '\r') ++begin;
    if (*begin == '\0') return false;

    errno = 0;
    char *end = nullptr;
    long value = strtol(begin, &end, 10);
    if (end == begin || errno == ERANGE) return false;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') ++end;
    if (*end != '\0') return false;
    if (value < -2147483648L || value > 2147483647L) return false;

    *out = static_cast<int>(value);
    return true;
}

std::string read_file_trimmed(const std::string &path) {
    std::string text = read_text_file(path);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.erase(text.begin());
    }
    return text;
}

std::string json_escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool dir_exists(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

}  // namespace

AppEntry::AppEntry() : order(1000), builtin(false), exclusive_runtime(false) {}

Theme::Theme()
    : id("tech_noir"),
      display_name("Tech Noir"),
      background_top{8, 14, 26},
      background_bottom{5, 9, 17},
      background_glow{44, 132, 208},
      background_wave_a{72, 120, 170},
      background_wave_b{28, 72, 126},
      panel_top{22, 33, 50},
      panel_bottom{14, 22, 35},
      panel_border{78, 106, 142},
      panel_focus_top{40, 68, 102},
      panel_focus_bottom{24, 44, 72},
      panel_focus_border{132, 184, 242},
      glow_soft{88, 144, 214},
      shadow{6, 10, 20},
      overlay{14, 18, 28},
      text_primary{233, 240, 250},
      text_muted{150, 172, 199},
      text_invert{16, 20, 30},
      accent{82, 194, 255},
      ok{112, 220, 142},
      warn{244, 188, 95},
      danger{246, 112, 122},
      footer_top{18, 28, 42},
      footer_bottom{11, 18, 28},
      footer_border{62, 84, 114},
      corner_radius(10),
      panel_alpha(232),
      background_motion(1) {}

Typography::Typography() : small_px(14), large_px(20), line_small(16), line_large(22), hint_chars(56) {}

LayoutMetrics::LayoutMetrics() : safe(), top_bar(), content(), footer(), gap(6), grid_cols(3), grid_rows(2) {}

Settings::Settings() : language("pl"), theme_id("midnight_violet"), volume(80), brightness(80), animations(true) {}

Profile::Profile() : username("Player1"), pin_enabled(false), pin("0000") {}

BatteryInfo::BatteryInfo()
    : available(false), has_capacity(false), has_voltage(false), capacity(0), voltage_mv(0), status("unknown"), node("-") {}

InputSnapshot::InputSnapshot()
    : hold_up(false), hold_down(false), hold_left(false), hold_right(false), hold_a(false), hold_b(false), hold_start(false),
      hold_select(false), hold_l(false), hold_r(false), hold_joy(false), nav_up(false), nav_down(false), nav_left(false),
      nav_right(false),
      accept(false), back(false), menu(false), axis_x(0), axis_y(0), axis_x_render(0), axis_y_render(0), edge_accept_ms(0),
      edge_back_ms(0), edge_menu_ms(0), edge_select_ms(0), edge_l_ms(0), edge_r_ms(0) {}

RuntimePerfStats::RuntimePerfStats()
    : frame_ms(0),
      update_ms(0),
      render_ms(0),
      present_ms(0),
      fps_avg(0.0f),
      fps_p95(0.0f),
      slow_frame_count(0),
      last_input_delay_a_ms(0),
      last_input_delay_b_ms(0),
      last_input_delay_start_ms(0),
      last_input_delay_select_ms(0),
      last_input_delay_l_ms(0),
      last_input_delay_r_ms(0) {}

UiRuntimeSnapshot::UiRuntimeSnapshot() : perf(), battery(), battery_percent(-1), scene_id(0), now_ms(0) {}

KeyboardKey::KeyboardKey() : id(), text(), row(0), col(0), span(1), action(false) {}

KeyboardLayout::KeyboardLayout() : cols(10), rows(4), letter_keys(), digit_keys() {}

TextInputState::TextInputState()
    : value(), max_len(12), key_index(0), key_cursor_t(0.0f), digits_mode(false), panel_t(0.0f), confirm_requested(false) {}

Logger::Logger(const std::string &path) : path_(path) {
    const size_t slash = path_.find_last_of('/');
    if (slash != std::string::npos) ensure_directory(path_.substr(0, slash));
}

void Logger::info(const std::string &msg) { log("INFO", msg); }
void Logger::warn(const std::string &msg) { log("WARN", msg); }
void Logger::error(const std::string &msg) { log("ERROR", msg); }

void Logger::log(const char *level, const std::string &msg) {
    FILE *f = fopen(path_.c_str(), "a");
    if (!f) return;
    fprintf(f, "%s [%s] %s\n", now_timestamp().c_str(), level, msg.c_str());
    fclose(f);
}

SettingsStore::SettingsStore(const std::string &path) : path_(path) {}

bool SettingsStore::load(Settings *settings, Logger *logger) const {
    std::string json = read_text_file(path_);
    if (json.empty()) return false;

    std::string language;
    std::string theme_id;
    int volume = 0;
    int brightness = 0;
    bool animations = false;

    if (extract_string(json, "language", &language)) settings->language = normalize_language(language);
    if (extract_string(json, "theme_id", &theme_id)) settings->theme_id = normalize_theme_id(theme_id);
    if (extract_int(json, "volume", &volume)) settings->volume = std::max(0, std::min(100, volume));
    if (extract_int(json, "brightness", &brightness)) settings->brightness = std::max(0, std::min(100, brightness));
    if (extract_bool(json, "animations", &animations)) settings->animations = animations;

    if (logger) logger->info("Loaded settings from " + path_);
    return true;
}

bool SettingsStore::save(const Settings &settings, Logger *logger) const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"language\": \"" << json_escape(normalize_language(settings.language)) << "\",\n";
    ss << "  \"theme_id\": \"" << json_escape(normalize_theme_id(settings.theme_id)) << "\",\n";
    ss << "  \"volume\": " << settings.volume << ",\n";
    ss << "  \"brightness\": " << settings.brightness << ",\n";
    ss << "  \"animations\": " << (settings.animations ? "true" : "false") << "\n";
    ss << "}\n";

    if (!write_text_file(path_, ss.str())) {
        if (logger) logger->error("Failed to save settings to " + path_);
        return false;
    }

    if (logger) logger->info("Saved settings to " + path_);
    return true;
}

bool SettingsStore::apply_volume(int volume, Logger *logger) const {
    struct MixerPercentControl {
        const char *name;
    };
    struct MixerSwitchControl {
        const char *name;
        const char *value;
    };

    int clamped = std::max(0, std::min(100, volume));
    const MixerPercentControl percent_controls[] = {
        {"Headphone Playback Volume"},
        {"DAC Front Playback Volume"},
        {"DAC Playback Volume"},
        {"Master"},
        {"Speaker"},
        {"Headphone"},
        {"PCM"},
        {"DAC"},
    };
    const MixerSwitchControl switch_controls[] = {
        {"Headphone Playback Switch", (clamped > 0) ? "on" : "off"},
        {"Speaker Playback Switch", (clamped > 0) ? "on" : "off"},
    };
    const char *debug_controls[] = {
        "Headphone Playback Volume",
        "DAC Front Playback Volume",
        "DAC Playback Volume",
        "Headphone Playback Switch",
        "Speaker Playback Switch",
    };

    bool applied = false;
    bool have_snd_control = (access("/dev/snd/controlC0", F_OK) == 0);
    std::vector<std::string> touched_controls;

    for (size_t i = 0; i < sizeof(percent_controls) / sizeof(percent_controls[0]); ++i) {
        const std::string quoted = shell_single_quote(percent_controls[i].name);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "amixer -q sset %s %d%% >/dev/null 2>&1",
                 quoted.c_str(), clamped);
        if (system(cmd) == 0) {
            applied = true;
            touched_controls.push_back(percent_controls[i].name);
        }
    }

    for (size_t i = 0; i < sizeof(switch_controls) / sizeof(switch_controls[0]); ++i) {
        const std::string quoted = shell_single_quote(switch_controls[i].name);
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "amixer -q sset %s %s >/dev/null 2>&1",
                 quoted.c_str(), switch_controls[i].value);
        if (system(cmd) == 0) {
            applied = true;
            touched_controls.push_back(switch_controls[i].name);
        }
    }

    if (logger) {
        if (applied) {
            std::ostringstream summary;
            summary << "Applied volume: " << clamped << "% via";
            for (size_t i = 0; i < touched_controls.size(); ++i) {
                summary << (i == 0 ? " " : ", ") << touched_controls[i];
            }
            logger->info(summary.str());

            for (size_t i = 0; i < sizeof(debug_controls) / sizeof(debug_controls[0]); ++i) {
                const std::string quoted = shell_single_quote(debug_controls[i]);
                int status = 0;
                std::string output = run_command_capture("amixer sget " + quoted + " 2>/dev/null", &status);
                if (status == 0 && !output.empty()) {
                    logger->info(std::string("Mixer state ") + debug_controls[i] + ": " + collapse_ws(output));
                }
            }
        } else {
            if (have_snd_control) {
                logger->warn("Unable to apply volume via amixer");
            } else {
                logger->warn("Unable to apply volume via amixer; /dev/snd/controlC0 is missing");
            }
        }
    }
    return applied;
}

bool SettingsStore::apply_brightness(int brightness, Logger *logger) const {
    int clamped = std::max(0, std::min(100, brightness));
    DIR *dir = opendir("/sys/class/backlight");
    bool applied = false;
    if (dir) {
        struct dirent *ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string base = std::string("/sys/class/backlight/") + ent->d_name;

            int max_brightness = 0;
            if (!read_file_int(base + "/max_brightness", &max_brightness) || max_brightness <= 0) continue;

            int value = (clamped * max_brightness) / 100;
            std::ostringstream ss;
            ss << value << "\n";
            if (write_text_file(base + "/brightness", ss.str())) {
                applied = true;
                if (logger) logger->info("Applied brightness " + std::to_string(clamped) + "% on " + ent->d_name);
                break;
            }
        }
        closedir(dir);
    } else if (logger) {
        logger->warn("Backlight sysfs not found; trying PWM fallback");
    }

    if (!applied) applied = write_pwm_backlight_percent(clamped, logger);
    if (!applied && logger) logger->warn("Unable to apply brightness");
    return applied;
}

ProfileStore::ProfileStore(const std::string &path) : path_(path) {}

bool ProfileStore::load(Profile *profile, Logger *logger) const {
    std::string json = read_text_file(path_);
    if (json.empty()) return false;

    std::string username;
    std::string pin;
    bool pin_enabled = false;

    if (extract_string(json, "username", &username)) profile->username = username;
    if (extract_string(json, "pin", &pin)) profile->pin = pin;
    if (extract_bool(json, "pin_enabled", &pin_enabled)) profile->pin_enabled = pin_enabled;

    if (logger) logger->info("Loaded profile from " + path_);
    return true;
}

bool ProfileStore::save(const Profile &profile, Logger *logger) const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"username\": \"" << json_escape(profile.username) << "\",\n";
    ss << "  \"pin_enabled\": " << (profile.pin_enabled ? "true" : "false") << ",\n";
    ss << "  \"pin\": \"" << json_escape(profile.pin) << "\"\n";
    ss << "}\n";

    if (!write_text_file(path_, ss.str())) {
        if (logger) logger->error("Failed to save profile to " + path_);
        return false;
    }

    if (logger) logger->info("Saved profile to " + path_);
    return true;
}

Registry::Registry() {}

void Registry::scan(const std::string &apps_dir, Logger *logger) {
    apps_.clear();
    if (!dir_exists(apps_dir)) {
        if (logger) logger->warn("Apps directory does not exist: " + apps_dir);
        return;
    }

    DIR *dir = opendir(apps_dir.c_str());
    if (!dir) {
        if (logger) logger->error("Cannot open apps directory: " + apps_dir);
        return;
    }

    std::set<std::string> seen_ids;

    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;

        std::string app_dir = apps_dir + "/" + ent->d_name;
        struct stat st;
        if (stat(app_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        std::string manifest_path = app_dir + "/app.json";
        std::string json = read_text_file(manifest_path);
        if (json.empty()) {
            if (logger) logger->warn("Skipping app without manifest: " + app_dir);
            continue;
        }

        AppEntry app;
        app.base_dir = app_dir;
        app.icon = "icon.png";
        app.exec = "launch.sh";
        app.order = 1000;

        if (!extract_string(json, "id", &app.id) || app.id.empty()) {
            if (logger) logger->warn("Skipping app with missing id: " + manifest_path);
            continue;
        }
        if (!extract_string(json, "name", &app.name) || app.name.empty()) {
            if (logger) logger->warn("Skipping app with missing name: " + app.id);
            continue;
        }

        extract_string(json, "type", &app.type);
        extract_string(json, "description", &app.description);
        extract_string(json, "icon", &app.icon);
        extract_string(json, "exec", &app.exec);
        extract_string(json, "category", &app.category);
        extract_int(json, "order", &app.order);
        extract_bool(json, "exclusive_runtime", &app.exclusive_runtime);
        app.args = extract_string_array(json, "args");
        if (app.id == "system.info") app.builtin = true;

        if (app.type.empty()) app.type = "game, gra";
        if (app.category.empty()) {
            if (app.type.find("game") != std::string::npos) app.category = "Games, Gry";
            else if (app.type.find("tool") != std::string::npos) app.category = "Tools, Narzędzia";
            else app.category = "Apps, Aplikacje";
        }

        if (app.icon.empty()) app.icon = "icon.png";
        if (app.exec.empty()) app.exec = "launch.sh";

        if (app.icon[0] != '/') app.icon = app_dir + "/" + app.icon;

        if (seen_ids.count(app.id) > 0) {
            if (logger) logger->warn("Skipping duplicated app id: " + app.id);
            continue;
        }
        seen_ids.insert(app.id);

        apps_.push_back(app);
    }

    closedir(dir);

    bool has_info = false;
    for (size_t i = 0; i < apps_.size(); ++i) {
        if (apps_[i].id == "system.info") {
            has_info = true;
            break;
        }
    }
    if (!has_info) {
        AppEntry info;
        info.id = "system.info";
        info.name = "System Info, Informacje systemowe";
        info.type = "tool, narzędzia";
        info.category = "System, System";
        info.icon = apps_dir + "/info/icon.png";
        info.exec = "/bin/true";
        info.order = 1;
        info.base_dir = apps_dir + "/info";
        info.builtin = true;
        info.exclusive_runtime = false;
        apps_.push_back(info);
    }

    const long ram_mib = read_mem_total_kb() / 1024;
    const long free_mib = free_disk_mib(apps_dir);
    std::string cpu = read_first_kv_value("/proc/cpuinfo", "model name");
    if (cpu.empty()) cpu = read_first_kv_value("/proc/cpuinfo", "Hardware");
    if (cpu.empty()) cpu = "Unknown";
    const int app_count = static_cast<int>(std::count_if(apps_.begin(), apps_.end(), [](const AppEntry &app) {
        return !app.builtin;
    }));
    for (size_t i = 0; i < apps_.size(); ++i) {
        if (apps_[i].id != "system.info") continue;
        std::ostringstream d;
        d << "CPU: " << cpu << ". RAM: " << ram_mib << " MiB. Wolne: " << free_mib
          << " MiB. Runtime: inconsole-runtime. Aplikacje: " << app_count
          << " | CPU: " << cpu << ". RAM: " << ram_mib << " MiB. Free: " << free_mib
          << " MiB. Runtime: inconsole-runtime. Apps: " << app_count;
        apps_[i].description = d.str();
        break;
    }

    std::sort(apps_.begin(), apps_.end(), [](const AppEntry &a, const AppEntry &b) {
        if (a.category != b.category) return a.category < b.category;
        if (a.order != b.order) return a.order < b.order;
        return a.name < b.name;
    });

    if (logger) logger->info("Loaded apps: " + std::to_string(apps_.size()));
}

std::vector<AppEntry> Registry::apps_for_category(const std::string &category) const {
    std::vector<AppEntry> out;
    for (size_t i = 0; i < apps_.size(); ++i) {
        if (apps_[i].category == category) out.push_back(apps_[i]);
    }
    return out;
}

const std::vector<AppEntry> &Registry::all_apps() const { return apps_; }

int ProcessRunner::run_app(const AppEntry &app, Logger *logger, std::string *error_text) const {
    pid_t pid = fork();
    if (pid < 0) {
        if (error_text) *error_text = "fork() failed";
        if (logger) logger->error("fork() failed for app: " + app.id);
        return -1;
    }

    if (pid == 0) {
        std::string exec_path = app.exec;
        if (!exec_path.empty() && exec_path[0] != '/') exec_path = app.base_dir + "/" + app.exec;

        if (chdir(app.base_dir.c_str()) != 0) _exit(127);

        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(exec_path.c_str()));
        for (size_t i = 0; i < app.args.size(); ++i) {
            argv.push_back(const_cast<char *>(app.args[i].c_str()));
        }
        argv.push_back(nullptr);

        execv(exec_path.c_str(), argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        if (error_text) *error_text = "waitpid() failed";
        if (logger) logger->error("waitpid() failed for app: " + app.id);
        return -1;
    }

    if (WIFEXITED(status)) {
        int rc = WEXITSTATUS(status);
        if (rc != 0 && error_text) *error_text = "app exited with code " + std::to_string(rc);
        if (logger) logger->info("App finished: " + app.id + " rc=" + std::to_string(rc));
        return rc;
    }

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (error_text) *error_text = "app killed by signal " + std::to_string(sig);
        if (logger) logger->warn("App killed by signal: " + app.id);
        return 128 + sig;
    }

    if (error_text) *error_text = "unknown app termination";
    if (logger) logger->warn("App terminated unexpectedly: " + app.id);
    return -1;
}

int ProcessRunner::run_app_exclusive(const AppEntry &app, Logger *logger, std::string *error_text) const {
    std::string exec_path = app.exec;
    if (!exec_path.empty() && exec_path[0] != '/') exec_path = app.base_dir + "/" + app.exec;
    if (exec_path.empty()) {
        if (error_text) *error_text = "missing executable path";
        return -1;
    }

    if (chdir(app.base_dir.c_str()) != 0) {
        if (error_text) *error_text = "chdir() failed";
        if (logger) logger->error("exclusive launch chdir failed for app: " + app.id);
        return -1;
    }

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(exec_path.c_str()));
    for (size_t i = 0; i < app.args.size(); ++i) {
        argv.push_back(const_cast<char *>(app.args[i].c_str()));
    }
    argv.push_back(nullptr);

    if (logger) logger->info("Exclusive launch: " + app.id + " exec=" + exec_path);
    execv(exec_path.c_str(), argv.data());

    if (error_text) *error_text = "execv() failed";
    if (logger) logger->error("Exclusive launch failed for app: " + app.id + " errno=" + std::to_string(errno));
    return -1;
}

BatteryMonitor::BatteryMonitor() : next_update_ms_(0), missing_samples_(0) {}

void BatteryMonitor::discover(Logger *logger) {
    battery_path_.clear();

    DIR *dir = opendir("/sys/class/power_supply");
    if (!dir) return;

    int best_score = -1000000;
    std::string best_path;
    std::string best_node;
    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string base = std::string("/sys/class/power_supply/") + ent->d_name;
        std::string type = read_file_trimmed(base + "/type");
        if (type != "Battery") continue;

        int score = 0;
        std::string node = ent->d_name;
        std::string node_lc = node;
        for (size_t i = 0; i < node_lc.size(); ++i) node_lc[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(node_lc[i])));

        if (node_lc.find("bq27220") != std::string::npos) score += 220;
        else if (node_lc.find("bq27") != std::string::npos) score += 140;
        else if (node_lc.find("axp") != std::string::npos) score -= 80;

        int cap = 0;
        const bool has_capacity = read_file_int(base + "/capacity", &cap);
        if (has_capacity && cap >= 0 && cap <= 100) score += 40;

        int uv = 0;
        const bool has_voltage = read_file_int(base + "/voltage_now", &uv) ||
                                 read_file_int(base + "/voltage_avg", &uv) ||
                                 read_file_int(base + "/voltage_ocv", &uv);
        if (has_voltage) {
            if (uv > 1000000) score += 60;
            else score -= 120;
        }
        const std::string status = read_file_trimmed(base + "/status");
        if (!status.empty()) score += 5;

        if (logger) {
            logger->info("Battery candidate: " + node + " cap=" + (has_capacity ? std::to_string(cap) : std::string("n/a")) +
                         " uv=" + (has_voltage ? std::to_string(uv) : std::string("n/a")) + " status=" +
                         (status.empty() ? std::string("-") : status) + " score=" + std::to_string(score));
        }

        if (score > best_score) {
            best_score = score;
            best_path = base;
            best_node = node;
        }
    }

    closedir(dir);
    if (!best_path.empty()) {
        battery_path_ = best_path;
        info_.node = best_node;
    }
    if (logger && !battery_path_.empty()) {
        logger->info("Battery node: " + info_.node + " score=" + std::to_string(best_score));
    }
}

const BatteryInfo &BatteryMonitor::update(Logger *logger) {
    const uint64_t now = monotonic_ms();
    if (now < next_update_ms_) return info_;
    next_update_ms_ = now + 1000;

    if (battery_path_.empty()) discover(logger);
    if (battery_path_.empty()) {
        info_.available = false;
        return info_;
    }

    info_.available = true;

    int capacity = 0;
    info_.has_capacity = read_file_int(battery_path_ + "/capacity", &capacity);
    if (info_.has_capacity) {
        if (capacity < 0 || capacity > 100) {
            info_.has_capacity = false;
        } else {
            info_.capacity = capacity;
        }
    }

    int uv = 0;
    info_.has_voltage = read_file_int(battery_path_ + "/voltage_now", &uv) ||
                        read_file_int(battery_path_ + "/voltage_avg", &uv) ||
                        read_file_int(battery_path_ + "/voltage_ocv", &uv);
    if (info_.has_voltage) {
        if (uv <= 0) {
            info_.has_voltage = false;
        } else {
            info_.voltage_mv = uv / 1000;
        }
    }

    std::string status = read_file_trimmed(battery_path_ + "/status");
    if (!status.empty()) info_.status = status;

    const bool clearly_invalid_sample = info_.has_capacity && info_.has_voltage &&
                                        info_.capacity == 0 && info_.voltage_mv <= 0;
    if ((!info_.has_capacity && !info_.has_voltage) || clearly_invalid_sample) {
        ++missing_samples_;
        if (missing_samples_ >= 5) {
            if (logger) logger->warn("Battery data missing; re-discovering power_supply battery node");
            battery_path_.clear();
            discover(logger);
            missing_samples_ = 0;
        }
    } else {
        missing_samples_ = 0;
    }

    return info_;
}

bool ensure_directory(const std::string &path) {
    if (path.empty()) return false;
    if (dir_exists(path)) return true;

    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        current.push_back(c);
        if (c != '/' && i + 1 != path.size()) continue;
        if (current.empty()) continue;
        if (current == "/") continue;

        struct stat st;
        if (stat(current.c_str(), &st) == 0) continue;
        if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }

    return true;
}

uint64_t monotonic_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec / 1000000ULL);
}

std::string read_text_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return std::string();

    std::string out;
    char buf[512];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, buf + n);
    }
    fclose(f);
    return out;
}

bool write_text_file(const std::string &path, const std::string &content) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        if (!ensure_directory(path.substr(0, slash))) return false;
    }

    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t written = fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return written == content.size();
}

std::string normalize_language(const std::string &language) {
    ensure_languages_loaded();
    std::string norm = language;
    for (size_t i = 0; i < norm.size(); ++i) {
        norm[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(norm[i])));
    }
    if (lang_cache().count(norm) > 0) return norm;
    return "pl";
}

std::vector<std::string> available_languages() {
    ensure_languages_loaded();
    return lang_order_cache();
}

std::string language_label(const std::string &language) {
    ensure_languages_loaded();
    const std::string norm = normalize_language(language);
    std::map<std::string, LangPack> &cache = lang_cache();
    std::map<std::string, LangPack>::const_iterator it = cache.find(norm);
    if (it != cache.end() && !it->second.name.empty()) return it->second.name;
    return norm;
}

std::string translate_ui_text(const std::string &language, const std::string &key, const std::string &fallback) {
    ensure_languages_loaded();
    const std::string norm = normalize_language(language);
    std::map<std::string, LangPack> &cache = lang_cache();
    std::map<std::string, LangPack>::const_iterator it = cache.find(norm);
    if (it == cache.end()) return fallback;
    std::map<std::string, std::string>::const_iterator sit = it->second.strings.find(key);
    if (sit == it->second.strings.end() || sit->second.empty()) return fallback;
    return sit->second;
}

std::string normalize_theme_id(const std::string &theme_id) {
    std::string norm;
    norm.reserve(theme_id.size());
    for (size_t i = 0; i < theme_id.size(); ++i) {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(theme_id[i])));
        if (c == '-') c = '_';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            norm.push_back(c);
        }
    }
    if (norm.empty()) return "midnight_violet";
    return norm;
}

std::vector<std::string> available_theme_ids() {
    if (theme_loaded_flag()) return theme_order_cache();
    theme_loaded_flag() = true;
    theme_cache().clear();
    theme_order_cache().clear();

    ThemePack tech = [] {
        ThemePack pack;
        pack.builtin = true;
        Theme t = Theme();
        t.id = "tech_noir";
        t.display_name = "Tech Noir";
        t.background_top = {16, 22, 34};
        t.background_bottom = {8, 12, 20};
        t.background_glow = {78, 120, 184};
        t.background_wave_a = {48, 92, 170};
        t.background_wave_b = {28, 64, 122};
        t.panel_top = {28, 34, 48};
        t.panel_bottom = {18, 22, 34};
        t.panel_border = {72, 88, 116};
        t.panel_focus_top = {44, 56, 82};
        t.panel_focus_bottom = {24, 34, 56};
        t.panel_focus_border = {124, 170, 226};
        t.glow_soft = {78, 132, 204};
        t.shadow = {4, 8, 14};
        t.overlay = {12, 18, 28};
        t.text_primary = {236, 240, 246};
        t.text_muted = {158, 172, 190};
        t.text_invert = {16, 20, 30};
        t.accent = {110, 176, 238};
        t.ok = {120, 210, 142};
        t.warn = {232, 186, 104};
        t.danger = {230, 110, 120};
        t.footer_top = {24, 30, 42};
        t.footer_bottom = {14, 18, 28};
        t.footer_border = {62, 76, 98};
        t.corner_radius = 11;
        t.panel_alpha = 226;
        t.background_motion = 1;
        pack.theme = t;
        return pack;
    }();

    ThemePack violet = [] {
        ThemePack pack;
        pack.builtin = true;
        Theme t = Theme();
        t.id = "midnight_violet";
        t.display_name = "Midnight Violet";
        t.background_top = {10, 8, 22};
        t.background_bottom = {4, 4, 11};
        t.background_glow = {82, 56, 164};
        t.background_wave_a = {106, 72, 198};
        t.background_wave_b = {42, 38, 116};
        t.panel_top = {28, 20, 42};
        t.panel_bottom = {16, 12, 28};
        t.panel_border = {88, 76, 136};
        t.panel_focus_top = {44, 28, 72};
        t.panel_focus_bottom = {22, 14, 38};
        t.panel_focus_border = {170, 142, 255};
        t.glow_soft = {126, 102, 240};
        t.shadow = {4, 2, 10};
        t.overlay = {18, 10, 28};
        t.text_primary = {244, 238, 255};
        t.text_muted = {178, 164, 212};
        t.text_invert = {16, 14, 22};
        t.accent = {170, 126, 255};
        t.ok = {118, 220, 160};
        t.warn = {244, 192, 108};
        t.danger = {242, 120, 144};
        t.footer_top = {22, 16, 34};
        t.footer_bottom = {12, 10, 20};
        t.footer_border = {70, 58, 108};
        t.corner_radius = 13;
        t.panel_alpha = 220;
        t.background_motion = 1;
        pack.theme = t;
        return pack;
    }();

    ThemePack light = [] {
        ThemePack pack;
        pack.builtin = true;
        Theme t = Theme();
        t.id = "graphite_light";
        t.display_name = "Graphite Light";
        t.background_top = {234, 238, 244};
        t.background_bottom = {216, 223, 232};
        t.background_glow = {178, 194, 214};
        t.background_wave_a = {196, 208, 228};
        t.background_wave_b = {174, 188, 212};
        t.panel_top = {252, 253, 255};
        t.panel_bottom = {236, 240, 247};
        t.panel_border = {156, 168, 186};
        t.panel_focus_top = {245, 250, 255};
        t.panel_focus_bottom = {222, 235, 248};
        t.panel_focus_border = {96, 136, 182};
        t.glow_soft = {140, 164, 208};
        t.shadow = {120, 132, 156};
        t.overlay = {208, 216, 228};
        t.text_primary = {24, 34, 48};
        t.text_muted = {82, 96, 118};
        t.text_invert = {248, 250, 255};
        t.accent = {64, 122, 182};
        t.ok = {42, 146, 92};
        t.warn = {188, 132, 42};
        t.danger = {186, 64, 76};
        t.footer_top = {232, 238, 246};
        t.footer_bottom = {220, 228, 238};
        t.footer_border = {152, 166, 186};
        t.corner_radius = 12;
        t.panel_alpha = 235;
        t.background_motion = 1;
        pack.theme = t;
        return pack;
    }();

    ThemePack retro = tech;
    retro.theme.id = "retro_arcade";
    retro.theme.display_name = "Retro Arcade";
    retro.theme.background_wave_a = {78, 44, 146};
    retro.theme.background_wave_b = {24, 58, 118};
    retro.theme.accent = {136, 108, 255};

    register_theme_pack(violet);
    register_theme_pack(tech);
    register_theme_pack(light);
    register_theme_pack(retro);

    DIR *dir = opendir(theme_dir_path().c_str());
    if (dir) {
        struct dirent *ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            const std::string subdir = ent->d_name;
            const std::string theme_path = theme_dir_path() + "/" + subdir + "/theme.json";
            const std::string json = read_text_file(theme_path);
            if (json.empty()) continue;

            ThemePack pack;
            pack.builtin = false;
            Theme theme = violet.theme;
            theme.id = normalize_theme_id(subdir);
            theme.display_name = theme.id;

            std::string id;
            std::string display_name;
            std::string motion;
            int corner_radius = 0;
            int panel_alpha = 0;
            if (extract_string(json, "id", &id) && !id.empty()) theme.id = normalize_theme_id(id);
            if (extract_string(json, "name", &display_name) && !display_name.empty()) theme.display_name = display_name;
            if (extract_string(json, "display_name", &display_name) && !display_name.empty()) theme.display_name = display_name;
            if (extract_color(json, "background_top", &theme.background_top)) {}
            if (extract_color(json, "background_bottom", &theme.background_bottom)) {}
            if (extract_color(json, "background_glow", &theme.background_glow)) {}
            if (extract_color(json, "background_wave_a", &theme.background_wave_a)) {}
            if (extract_color(json, "background_wave_b", &theme.background_wave_b)) {}
            if (extract_color(json, "panel_top", &theme.panel_top)) {}
            if (extract_color(json, "panel_bottom", &theme.panel_bottom)) {}
            if (extract_color(json, "panel_border", &theme.panel_border)) {}
            if (extract_color(json, "panel_focus_top", &theme.panel_focus_top)) {}
            if (extract_color(json, "panel_focus_bottom", &theme.panel_focus_bottom)) {}
            if (extract_color(json, "panel_focus_border", &theme.panel_focus_border)) {}
            if (extract_color(json, "glow_soft", &theme.glow_soft)) {}
            if (extract_color(json, "shadow", &theme.shadow)) {}
            if (extract_color(json, "overlay", &theme.overlay)) {}
            if (extract_color(json, "text_primary", &theme.text_primary)) {}
            if (extract_color(json, "text_muted", &theme.text_muted)) {}
            if (extract_color(json, "text_invert", &theme.text_invert)) {}
            if (extract_color(json, "accent", &theme.accent)) {}
            if (extract_color(json, "ok", &theme.ok)) {}
            if (extract_color(json, "warn", &theme.warn)) {}
            if (extract_color(json, "danger", &theme.danger)) {}
            if (extract_color(json, "footer_top", &theme.footer_top)) {}
            if (extract_color(json, "footer_bottom", &theme.footer_bottom)) {}
            if (extract_color(json, "footer_border", &theme.footer_border)) {}
            if (extract_int(json, "corner_radius", &corner_radius)) theme.corner_radius = std::max(0, std::min(24, corner_radius));
            if (extract_int(json, "panel_alpha", &panel_alpha)) theme.panel_alpha = std::max(96, std::min(255, panel_alpha));
            if (extract_string(json, "background_motion", &motion)) theme.background_motion = parse_theme_motion(motion) ? 1 : 0;
            pack.theme = theme;
            register_theme_pack(pack);
        }
        closedir(dir);
    }

    return theme_order_cache();
}

std::string theme_label(const std::string &theme_id) {
    available_theme_ids();
    const std::string norm = normalize_theme_id(theme_id);
    std::map<std::string, ThemePack> &cache = theme_cache();
    std::map<std::string, ThemePack>::const_iterator it = cache.find(norm);
    if (it != cache.end() && !it->second.theme.display_name.empty()) return it->second.theme.display_name;
    return norm;
}

const Theme &theme_by_id(const std::string &theme_id) {
    available_theme_ids();
    const std::string norm = normalize_theme_id(theme_id);
    std::map<std::string, ThemePack> &cache = theme_cache();
    std::map<std::string, ThemePack>::const_iterator it = cache.find(norm);
    if (it != cache.end()) return it->second.theme;
    return cache.find("midnight_violet")->second.theme;
}

const Typography &default_typography() {
    static Typography t;
    return t;
}

LayoutMetrics build_layout_metrics(int screen_w, int screen_h) {
    LayoutMetrics m;
    const int margin = 8;
    m.safe = LayoutRect(margin, margin, screen_w - margin * 2, screen_h - margin * 2);
    m.gap = 6;
    m.grid_cols = 3;
    m.grid_rows = 2;

    m.top_bar = LayoutRect(m.safe.x, m.safe.y, m.safe.w, 34);
    m.footer = LayoutRect(m.safe.x, m.safe.bottom() - 24, m.safe.w, 24);
    const int content_y = m.top_bar.bottom() + m.gap;
    const int content_h = m.footer.y - m.gap - content_y;
    m.content = LayoutRect(m.safe.x, content_y, m.safe.w, std::max(1, content_h));
    return m;
}

}  // namespace inconsole
