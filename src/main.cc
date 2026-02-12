#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>

#include "siegedb/siegedb.hh"

static std::unordered_map<std::string, std::string> LoadDotEnv(
    const std::string& path) {
    std::unordered_map<std::string, std::string> vars;
    std::ifstream file(path);
    if (!file.is_open()) {
        return vars;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Trim surrounding whitespace from key
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) {
            key.erase(key.begin());
        }

        // Strip optional surrounding quotes from value
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty()) {
            vars[key] = value;
        }
    }
    return vars;
}

static std::string GetEnvVar(
    const std::string& name,
    const std::unordered_map<std::string, std::string>& dotenv) {
    // Environment variables take precedence over .env file
    if (const char* val = std::getenv(name.c_str())) {
        return val;
    }
    auto it = dotenv.find(name);
    if (it != dotenv.end()) {
        return it->second;
    }
    return "";
}

int main() {
    auto dotenv = LoadDotEnv(".env");

    std::string api_url = GetEnvVar("SIEGEDB_API_URL", dotenv);
    std::string api_token = GetEnvVar("SIEGEDB_API_TOKEN", dotenv);

    if (api_url.empty() || api_token.empty()) {
        printf("Error: SIEGEDB_API_URL and SIEGEDB_API_TOKEN must be set\n");
        printf("Set them as environment variables or in a .env file\n");
        return 1;
    }

    auto siegedb = siegedb::SiegeDB::New(api_url, api_token);
    if (!siegedb) {
        printf("Failed to initialize SiegeDB\n");
        return 1;
    }

    if (!siegedb->Attach("RainbowSix.exe")) {
        printf("Failed to attach to process\n");
        return 1;
    }

    // specific classes/fields:
    /*auto offsets = siegedb->GetOffsets(
        "classes=Entity{m_BoundingVolume,m_Components},BoundingVolume{m_Min,m_"
        "Max}");*/
    // whole dump:
    auto offsets = siegedb->GetOffsets();
    if (offsets.is_null()) {
        printf("Failed to get offsets\n");
        return 1;
    }

    printf("Offsets: %s\n", offsets.dump(4).c_str());
    return 0;
}
