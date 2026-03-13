#include "siegedb/api.hh"

#include <cstdio>
#include <format>
#include <memory>
#include <string>

#include "http/http.hh"

namespace siegedb {
    std::unique_ptr<Api> Api::New(const std::string& url,
                                  const std::string& token) {
        std::unique_ptr<Api> api(new Api());
        api->http_ = http::Http::New();
        if (!api->http_) {
            printf("[siegedb::Api::New] failed to create Http client\n");
            return nullptr;
        }
        api->http_->SetHeader("Authorization", "Bearer " + token);
        api->url_ = url;
        if (!api->CheckHealth()) {
            printf("[siegedb::Api::New] health check did not pass\n");
            return nullptr;
        }
        return api;
    }

    Api::OffsetsResponse Api::GetOffsets(uint32_t timestamp,
                                         const std::string& query) {
        const std::string params = !query.empty() ? "?" + query : "";
        auto res = Get("/offsets/" + std::format("{:08x}", timestamp) + params);
        if (!res.error.empty()) {
            printf("[siegedb::Api::GetOffsets] request failed: %s\n",
                   res.error.c_str());
            return {OffsetsResponse::Type::FAIL};
        }
        const std::string status = res.body.value("status", "");
        switch (res.status_code) {
            case 200:
                return {OffsetsResponse::Type::SUCCESS, res.body};
            case 202:
                if (status == "processing") {
                    return {OffsetsResponse::Type::WAIT};
                }
                if (status == "awaiting_data") {
                    auto& dr = res.body["data_request"];
                    std::vector<SizeRange> ranges;
                    if (dr.contains("size_ranges") && dr["size_ranges"].is_array()) {
                        for (const auto& r : dr["size_ranges"]) {
                            ranges.push_back({r.value("min_size", size_t{0}),
                                              r.value("max_size", size_t{0})});
                        }
                    }
                    std::vector<DirectRead> reads;
                    if (dr.contains("reads") && dr["reads"].is_array()) {
                        for (const auto& rd : dr["reads"]) {
                            reads.push_back(
                                {rd.value("address", uint64_t{0}),
                                 rd.value("size", size_t{0})});
                        }
                    }
                    return {OffsetsResponse::Type::SEND_DATA,
                            std::nullopt,
                            std::nullopt,
                            res.body.value("job_id", ""),
                            std::move(ranges),
                            std::move(reads)};
                }
                return {OffsetsResponse::Type::UNKNOWN};
            case 404:
                printf("[siegedb::Api::GetOffsets] 404: %s - %s\n",
                       res.body.value("error", "unknown").c_str(),
                       res.body.value("message", "").c_str());
                return {OffsetsResponse::Type::NOT_FOUND, std::nullopt,
                        res.body.value("upload_token", "")};
            default:
                printf("[siegedb::Api::GetOffsets] unexpected status %ld: %s\n",
                       res.status_code, res.body.dump().c_str());
                return {OffsetsResponse::Type::FAIL};
        }
    }

    bool Api::Upload(const std::string& upload_token, const uint8_t* data,
                     size_t size, std::string& job_id) {
        http_->SetHeader("X-Upload-Token", upload_token);
        auto res = PostRaw("/upload", data, size, true);
        if (!res.error.empty() || res.status_code != 202) {
            printf("[siegedb::Api::Upload] upload failed: %s\n",
                   res.error.empty() ? "unexpected status code"
                                     : res.error.c_str());
            return false;
        }
        job_id = res.body.value("job_id", "");
        return !job_id.empty();
    }

    bool Api::UploadMemory(const std::string& job_id, const uint8_t* data,
                           size_t size, std::string& new_job_id) {
        auto res = PostRaw("/memory/" + job_id, data, size, true);
        if (!res.error.empty() || res.status_code != 202) {
            printf("[siegedb::Api::UploadMemory] upload failed: %s\n",
                   res.error.empty() ? "unexpected status code"
                                     : res.error.c_str());
            return false;
        }
        new_job_id = res.body.value("job_id", "");
        return !new_job_id.empty();
    }

    Api::StatusResponse Api::GetStatus(const std::string& job_id) {
        auto res = Get("/status/" + job_id);
        if (!res.error.empty()) {
            printf("[siegedb::Api::GetStatus] request failed: %s\n",
                   res.error.c_str());
            return {StatusResponse::Status::FAILED, job_id, res.error};
        }
        const std::string status = res.body.value("status", "");
        StatusResponse::Status s = StatusResponse::Status::UNKNOWN;
        if (status == "pending") {
            s = StatusResponse::Status::PENDING;
        } else if (status == "processing") {
            s = StatusResponse::Status::PROCESSING;
        } else if (status == "awaiting_data") {
            s = StatusResponse::Status::AWAITING_DATA;
        } else if (status == "complete") {
            s = StatusResponse::Status::COMPLETE;
        } else if (status == "failed") {
            s = StatusResponse::Status::FAILED;
        }

        StatusResponse sr{s, res.body.value("job_id", job_id),
                          res.body.value("error_message", "")};
        if (s == StatusResponse::Status::AWAITING_DATA &&
            res.body.contains("data_request")) {
            auto& dr = res.body["data_request"];
            if (dr.contains("size_ranges") && dr["size_ranges"].is_array()) {
                for (const auto& r : dr["size_ranges"]) {
                    sr.size_ranges.push_back(
                        {r.value("min_size", size_t{0}),
                         r.value("max_size", size_t{0})});
                }
            }
            if (dr.contains("reads") && dr["reads"].is_array()) {
                for (const auto& rd : dr["reads"]) {
                    sr.reads.push_back(
                        {rd.value("address", uint64_t{0}),
                         rd.value("size", size_t{0})});
                }
            }
        }
        return sr;
    }

    std::optional<std::string> Api::InitUpload(
        const std::string& upload_token, uint32_t chunk_total) {
        http_->SetHeader("X-Upload-Token", upload_token);
        nlohmann::json body = {{"chunk_total", chunk_total}};
        auto res = Post("/upload/init", body);
        if (!res.error.empty() || res.status_code != 202) {
            printf("[siegedb::Api::InitUpload] init failed: %s\n",
                   res.error.empty() ? "unexpected status code"
                                     : res.error.c_str());
            return std::nullopt;
        }
        std::string upload_id = res.body.value("upload_id", "");
        if (upload_id.empty()) {
            printf("[siegedb::Api::InitUpload] no upload_id in response\n");
            return std::nullopt;
        }
        return upload_id;
    }

    bool Api::UploadChunk(const std::string& upload_id, uint32_t chunk_index,
                          const uint8_t* data, size_t size) {
        auto res = PostRaw(
            "/upload/" + upload_id + "/" + std::to_string(chunk_index),
            data, size, false);
        if (!res.error.empty() ||
            (res.status_code != 200 && res.status_code != 202)) {
            printf("[siegedb::Api::UploadChunk] chunk %u failed: %s\n",
                   chunk_index,
                   res.error.empty() ? "unexpected status code"
                                     : res.error.c_str());
            return false;
        }
        return true;
    }

    std::string Api::GetAuthHeader() const {
        for (const auto& h : http_->GetHeaders()) {
            if (h.rfind("Authorization: ", 0) == 0) {
                return h;
            }
        }
        return "";
    }

    std::string Api::GetUrl() const { return url_; }

    Api::Api() : http_(nullptr) {}

    bool Api::CheckHealth() {
        auto health = Get("/health");
        return health.status_code == 200;
    }

    http::Response Api::Get(const std::string& endpoint) {
        return http_->Get(url_ + endpoint);
    }
    http::Response Api::Post(const std::string& endpoint,
                             const nlohmann::json& body) {
        return http_->Post(url_ + endpoint, body);
    }
    http::Response Api::PostRaw(const std::string& endpoint,
                                const uint8_t* data, size_t size,
                                bool compressed) {
        return http_->PostRaw(url_ + endpoint, data, size, compressed);
    }
}  // namespace siegedb
