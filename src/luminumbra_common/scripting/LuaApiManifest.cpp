#include "LuaApiManifest.h"

#include <cstddef>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>

namespace Luminumbra::scripting {
namespace {

constexpr const char* kLuaApiManifestSchema = "luminumbra.scripting.lua_api_manifest.v1";
constexpr const char* kDeterministicOrder = "module_then_name";

void AppendJsonString(std::ostringstream& out, const std::string& value) {
    out << '"';
    for (const unsigned char c : value) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c) << std::dec;
                } else {
                    out << static_cast<char>(c);
                }
                break;
        }
    }
    out << '"';
}

bool ContainsEntry(const LuaApiManifest& manifest, const char* module, const char* name) {
    for (const auto& entry : manifest.entries) {
        if (entry.module == module && entry.name == name) {
            return true;
        }
    }
    return false;
}

bool IsSortedByModuleThenName(const LuaApiManifest& manifest) {
    for (std::size_t i = 1; i < manifest.entries.size(); ++i) {
        const auto& previous = manifest.entries[i - 1];
        const auto& current = manifest.entries[i];
        if (previous.module > current.module) {
            return false;
        }
        if (previous.module == current.module && previous.name > current.name) {
            return false;
        }
    }
    return true;
}

} // namespace

const LuaApiManifest& GetLuaApiManifest() {
    static const LuaApiManifest manifest{
        kLuaApiManifestSchema,
        "1.1.0",
        kDeterministicOrder,
        {
            {"",
             "sample_energy_field",
             "function",
             "sample_energy_field(x: number, y: number, z: number) -> number",
             "Read-only energy-field sampler; alias of world.sample_energy_field."},
            {"world",
             "sample_energy_field",
             "function",
             "world.sample_energy_field(x: number, y: number, z: number) -> number",
             "Read-only sample of the stateful energy field at a world position, in gameplay "
             "units; 0 when the layer is absent."},
        },
    };
    return manifest;
}

std::string SerializeLuaApiManifestJson(const LuaApiManifest& manifest) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": ";
    AppendJsonString(out, manifest.schema);
    out << ",\n  \"manifest_version\": ";
    AppendJsonString(out, manifest.manifest_version);
    out << ",\n  \"deterministic_order\": ";
    AppendJsonString(out, manifest.deterministic_order);
    out << ",\n  \"entries\": [\n";
    for (std::size_t i = 0; i < manifest.entries.size(); ++i) {
        const auto& entry = manifest.entries[i];
        out << "    {\"module\": ";
        AppendJsonString(out, entry.module);
        out << ", \"name\": ";
        AppendJsonString(out, entry.name);
        out << ", \"kind\": ";
        AppendJsonString(out, entry.kind);
        out << ", \"signature\": ";
        AppendJsonString(out, entry.signature);
        out << ", \"description\": ";
        AppendJsonString(out, entry.description);
        out << "}";
        if (i + 1 < manifest.entries.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

bool LuaApiManifestMeetsBaseline(const LuaApiManifest& manifest) {
    if (manifest.schema != kLuaApiManifestSchema) {
        return false;
    }
    if (manifest.deterministic_order != kDeterministicOrder) {
        return false;
    }
    if (!IsSortedByModuleThenName(manifest)) {
        return false;
    }

    // This baseline is the current read-only host, including its bare alias.
    // Adding an operation requires changing the host and its live tests too.
    constexpr const char* requiredEntries[][2] = {
        {"", "sample_energy_field"},
        {"world", "sample_energy_field"},
    };
    if (manifest.manifest_version != "1.1.0" || manifest.entries.size() != 2) {
        return false;
    }
    for (const auto& entry : manifest.entries) {
        if (entry.kind != "function" || entry.signature.empty()) {
            return false;
        }
    }

    for (const auto& requiredEntry : requiredEntries) {
        if (!ContainsEntry(manifest, requiredEntry[0], requiredEntry[1])) {
            return false;
        }
    }
    return true;
}

} // namespace Luminumbra::scripting
