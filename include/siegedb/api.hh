#ifndef SIEGEDB_CLIENT_SIEGEDB_API_HH_
#define SIEGEDB_CLIENT_SIEGEDB_API_HH_

#include <cstdint>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

#include "http/http.hh"

namespace siegedb {
    struct SizeRange {
        size_t min_size = 0;
        size_t max_size = 0;
    };

    struct DirectRead {
        uint64_t address = 0;
        size_t size = 0;
    };

    class Api {
    public:
        struct OffsetsResponse {
            enum class Type {
                SUCCESS,
                FAIL,
                NOT_FOUND,
                WAIT,
                SEND_DATA,
                UNKNOWN
            };

            Type type;
            std::optional<nlohmann::json> data;
            std::optional<std::string> upload_token;
            std::string job_id;
            std::vector<SizeRange> size_ranges;
            std::vector<DirectRead> reads;
        };

        struct StatusResponse {
            enum class Status {
                PENDING,
                PROCESSING,
                AWAITING_DATA,
                COMPLETE,
                FAILED,
                UNKNOWN
            };

            Status status;
            std::string job_id;
            std::string error_message;
            std::vector<SizeRange> size_ranges;
            std::vector<DirectRead> reads;
        };

        Api(const Api&) = delete;
        Api& operator=(const Api&) = delete;
        Api(Api&& other) noexcept = default;
        Api& operator=(Api&& other) noexcept = default;

        static std::unique_ptr<Api> New(const std::string& url,
                                        const std::string& token);

        OffsetsResponse GetOffsets(uint32_t timestamp,
                                   const std::string& query = "");
        bool Upload(const std::string& upload_token, const uint8_t* data,
                    size_t size, std::string& job_id);
        bool UploadMemory(const std::string& job_id, const uint8_t* data,
                          size_t size, std::string& new_job_id);
        StatusResponse GetStatus(const std::string& job_id);

        std::optional<std::string> InitUpload(
            const std::string& upload_token, uint32_t chunk_total);
        bool UploadChunk(const std::string& upload_id, uint32_t chunk_index,
                         const uint8_t* data, size_t size);

        std::string GetAuthHeader() const;
        std::string GetUrl() const;

    private:
        Api();

        bool CheckHealth();

        http::Response Get(const std::string& endpoint);
        http::Response Post(const std::string& endpoint,
                            const nlohmann::json& body);
        http::Response PostRaw(const std::string& endpoint, const uint8_t* data,
                               size_t size, bool compressed = false);

        std::unique_ptr<http::Http> http_;
        std::string url_;
    };
}  // namespace siegedb

#endif  // SIEGEDB_CLIENT_SIEGEDB_API_HH_
