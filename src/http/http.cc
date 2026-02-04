#include "http/http.hh"

#include <cstdio>

namespace http {
    Http::Http(Http&& other) noexcept : curl_(other.curl_) {
        other.curl_ = nullptr;
    }
    Http& Http::operator=(Http&& other) noexcept {
        if (this != &other) {
            if (curl_) {
                curl_easy_cleanup(curl_);
            }
            curl_ = other.curl_;
            other.curl_ = nullptr;
        }
        return *this;
    }

    Http::~Http() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    std::unique_ptr<Http> Http::New() {
        std::unique_ptr<Http> http(new Http());
        http->curl_ = curl_easy_init();
        if (!http->curl_) {
            return nullptr;
        }
        return http;
    }

    void Http::SetHeader(const std::string& key, const std::string& value) {
        headers_.push_back(key + ": " + value);
    }

    Response Http::Get(const std::string& url) { return Perform(url, nullptr); }
    Response Http::Post(const std::string& url, const nlohmann::json& body) {
        std::string data = body.dump();
        return Perform(url, &data);
    }
    Response Http::PostRaw(const std::string& url, const uint8_t* data,
                           size_t size) {
        return Perform(url, nullptr, data, size);
    }

    Http::Http() : curl_(nullptr), upload_done_(false) {}

    size_t Http::WriteCallback(char* ptr, size_t size, size_t nmemb,
                               void* userdata) {
        auto* response = static_cast<std::string*>(userdata);
        response->append(ptr, size * nmemb);
        return size * nmemb;
    }
    int Http::ProgressCallback(void* clientp, curl_off_t dltotal,
                               curl_off_t dlnow, curl_off_t ultotal,
                               curl_off_t ulnow) {
        (void)dltotal;
        (void)dlnow;
        auto* self = static_cast<Http*>(clientp);
        if (ultotal <= 0 || self->upload_done_) {
            return 0;
        }
        double pct =
            static_cast<double>(ulnow) / static_cast<double>(ultotal) * 100.0;
        printf("\r[upload] %.1f / %.1f MB (%.0f%%)",
               static_cast<double>(ulnow) / (1024.0 * 1024.0),
               static_cast<double>(ultotal) / (1024.0 * 1024.0), pct);
        fflush(stdout);
        if (ulnow == ultotal) {
            printf("\n");
            self->upload_done_ = true;
        }
        return 0;
    }
    Response Http::Perform(const std::string& url, const std::string* post_data,
                           const uint8_t* raw_data, size_t raw_size) {
        curl_easy_reset(curl_);

        std::string response_body;

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl_, CURLOPT_UPLOAD_BUFFERSIZE, 2L * 1024 * 1024);

        struct curl_slist* headers = nullptr;
        if (raw_data) {
            headers = curl_slist_append(
                headers, "Content-Type: application/octet-stream");
        } else {
            headers =
                curl_slist_append(headers, "Content-Type: application/json");
        }
        headers = curl_slist_append(headers, "Accept: application/json");
        for (const auto& header : headers_) {
            headers = curl_slist_append(headers, header.c_str());
        }
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

        upload_done_ = false;
        curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
        curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);

        if (raw_data) {
            curl_easy_setopt(curl_, CURLOPT_POST, 1L);
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, raw_data);
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(raw_size));
        } else if (post_data) {
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, post_data->c_str());
        }

        CURLcode res = curl_easy_perform(curl_);
        curl_slist_free_all(headers);

        if (res != CURLE_OK) {
            return {0, nullptr, curl_easy_strerror(res)};
        }

        long status_code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status_code);

        nlohmann::json parsed =
            nlohmann::json::parse(response_body, nullptr, false);
        if (parsed.is_discarded()) {
            std::string preview = response_body.substr(0, 256);
            return {status_code, nullptr,
                    "Failed to parse response body as JSON: " + preview};
        }

        return {status_code, std::move(parsed), ""};
    }
}  // namespace http
