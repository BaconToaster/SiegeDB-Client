#ifndef SIEGEDB_CLIENT_SIEGEDB_SIEGEDB_HH_
#define SIEGEDB_CLIENT_SIEGEDB_SIEGEDB_HH_

#include <Windows.h>
#include <windef.h>
#include <winnt.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "siegedb/api.hh"

namespace siegedb {
    class SiegeDB {
    public:
        SiegeDB(const SiegeDB&) = delete;
        SiegeDB& operator=(const SiegeDB&) = delete;
        SiegeDB(SiegeDB&& other) noexcept = default;
        SiegeDB& operator=(SiegeDB&& other) noexcept = default;

        static std::unique_ptr<SiegeDB> New(const std::string& api_url,
                                            const std::string& api_token);

        bool Attach(const std::string& process_name);
        nlohmann::json GetOffsets(const std::string& query = "");

    private:
        SiegeDB();

        uint32_t GetProcessId(const std::string& process_name);
        uint64_t GetModuleBase(const std::string& module_name);
        IMAGE_NT_HEADERS64 GetNtHeaders();

        bool ReadDump(std::vector<uint8_t>& out);
        bool ReadHeapRegions(size_t min_size, size_t max_size,
                             std::vector<uint8_t>& out);
        bool PollUntilDone(const std::string& job_id, uint32_t timestamp,
                           const std::string& query, nlohmann::json& result);

        std::unique_ptr<Api> api_;
        uint32_t pid_;
        HANDLE h_proc_;
        uint64_t image_base_;
    };
}  // namespace siegedb

#endif  // SIEGEDB_CLIENT_SIEGEDB_SIEGEDB_HH_
