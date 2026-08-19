#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "uci_settings.h"

#ifdef OPENWRT_BUILD
extern "C" {
#include <uci.h>
}
#endif

namespace {

void copy_iface(char* dest, size_t dest_len, const char* src) {
    if (!dest || dest_len == 0) {
        return;
    }
    if (!src) {
        src = "lan";
    }

    size_t i = 0;
    for (; i + 1 < dest_len && src[i] != '\0'; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

void apply_defaults(UciSettings& out) {
    out.enabled = true;
    out.port = 5005;
    copy_iface(out.iface, sizeof(out.iface), "lan");
    out.policy.reset_to_defaults();
}

bool parse_enabled(const char* val, bool fallback) {
    if (!val || val[0] == '\0') {
        return fallback;
    }
    if (std::strcmp(val, "0") == 0 ||
        std::strcmp(val, "off") == 0 ||
        std::strcmp(val, "false") == 0 ||
        std::strcmp(val, "no") == 0 ||
        std::strcmp(val, "disabled") == 0) {
        return false;
    }
    if (std::strcmp(val, "1") == 0 ||
        std::strcmp(val, "on") == 0 ||
        std::strcmp(val, "true") == 0 ||
        std::strcmp(val, "yes") == 0 ||
        std::strcmp(val, "enabled") == 0) {
        return true;
    }
    return fallback;
}

bool parse_port(const char* val, uint16_t& port) {
    if (!val || val[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    unsigned long parsed = std::strtoul(val, &end, 10);
    if (end == val || !end || *end != '\0' || parsed < 1 || parsed > 65535) {
        return false;
    }

    port = static_cast<uint16_t>(parsed);
    return true;
}

bool parse_lux_below(const char* val, uint16_t& lux_below) {
    return parse_port(val, lux_below);
}

bool parse_brightness(const char* val, uint8_t& brightness) {
    if (!val || val[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    unsigned long parsed = std::strtoul(val, &end, 10);
    if (end == val || !end || *end != '\0' || parsed > 100) {
        return false;
    }

    brightness = static_cast<uint8_t>(parsed);
    return true;
}

} // namespace

#ifdef OPENWRT_BUILD

bool load_uci_settings(UciSettings& out) {
    apply_defaults(out);

    struct uci_context* ctx = uci_alloc_context();
    if (!ctx) {
        return false;
    }

    struct uci_package* pkg = nullptr;
    if (uci_load(ctx, "light_control", &pkg) != UCI_OK || !pkg) {
        uci_free_context(ctx);
        return false;
    }

    BrightnessCurve loaded_default;
    loaded_default.clear();
    BrightnessPolicy loaded_named;
    loaded_named.clear_named();
    SceneScheduler loaded_scenes;
    loaded_scenes.clear();

    struct uci_element* e = nullptr;
    uci_foreach_element(&pkg->sections, e) {
        struct uci_section* s = uci_to_section(e);
        if (!s) {
            continue;
        }

        if (std::strcmp(s->type, "light_control") == 0) {
            const char* val = uci_lookup_option_string(ctx, s, "enabled");
            out.enabled = parse_enabled(val, out.enabled);

            val = uci_lookup_option_string(ctx, s, "port");
            uint16_t port = out.port;
            if (parse_port(val, port)) {
                out.port = port;
            }

            val = uci_lookup_option_string(ctx, s, "interface");
            if (val && val[0] != '\0') {
                copy_iface(out.iface, sizeof(out.iface), val);
            }
            continue;
        }

        if (std::strcmp(s->type, "map") == 0) {
            const char* lux_val = uci_lookup_option_string(ctx, s, "lux_below");
            const char* br_val = uci_lookup_option_string(ctx, s, "brightness");
            uint16_t lux_below = 0;
            uint8_t brightness = 0;
            if (!parse_lux_below(lux_val, lux_below) ||
                !parse_brightness(br_val, brightness)) {
                continue;
            }

            const char* set_val = uci_lookup_option_string(ctx, s, "set");
            if (set_val && set_val[0] != '\0') {
                loaded_named.add_named_entry(set_val, lux_below, brightness);
            } else {
                loaded_default.add(lux_below, brightness);
            }
            continue;
        }

        if (std::strcmp(s->type, "scene") != 0) {
            continue;
        }

        const char* from_val = uci_lookup_option_string(ctx, s, "from_time");
        const char* to_val = uci_lookup_option_string(ctx, s, "to_time");
        uint16_t from_min = 0;
        uint16_t to_min = 0;
        if (!parse_hhmm(from_val, from_min) || !parse_hhmm(to_val, to_min)) {
            continue;
        }

        Scene scene;
        copy_iface(scene.name, sizeof(scene.name), e->name);
        scene.from_min = from_min;
        scene.to_min = to_min;

        const char* map_set = uci_lookup_option_string(ctx, s, "map_set");
        if (map_set && map_set[0] != '\0') {
            copy_iface(scene.map_set, sizeof(scene.map_set), map_set);
        }

        uint8_t min_brightness = 0;
        const char* min_val = uci_lookup_option_string(ctx, s, "min_brightness");
        if (parse_brightness(min_val, min_brightness)) {
            scene.has_min = true;
            scene.min_brightness = min_brightness;
        }

        uint8_t max_brightness = 100;
        const char* max_val = uci_lookup_option_string(ctx, s, "max_brightness");
        if (parse_brightness(max_val, max_brightness)) {
            scene.has_max = true;
            scene.max_brightness = max_brightness;
        }

        loaded_scenes.add(scene);
    }

    if (loaded_default.size() > 0) {
        loaded_default.sort();
        out.policy.set_default_curve(loaded_default);
    }

    if (loaded_named.named_count() > 0) {
        loaded_named.sort_named();
        out.policy.clear_named();
        for (size_t i = 0; i < loaded_named.named_count(); ++i) {
            const BrightnessPolicy::NamedCurve& named = loaded_named.named_at(i);
            for (size_t j = 0; j < named.curve.size(); ++j) {
                const BrightnessCurve::Entry& entry = named.curve.at(j);
                out.policy.add_named_entry(named.name, entry.lux_below, entry.brightness);
            }
        }
        out.policy.sort_named();
    }

    if (loaded_scenes.size() > 0) {
        out.policy.scheduler() = loaded_scenes;
    }

    uci_free_context(ctx);
    return true;
}

#else

bool load_uci_settings(UciSettings& out) {
    apply_defaults(out);
    return false;
}

#endif
