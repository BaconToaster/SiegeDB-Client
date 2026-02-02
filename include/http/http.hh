#ifndef SIEGEDB_CLIENT_HTTP_HTTP_HH_
#define SIEGEDB_CLIENT_HTTP_HTTP_HH_

#include <curl/curl.h>

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace http {
    struct Response {
        long status_code;
        nlohmann::json body;
        std::string error;
    };

    class Http {
    public:
        Http(const Http&) = delete;
        Http& operator=(const Http&) = delete;
        Http(Http&& other) noexcept;
        Http& operator=(Http&& other) noexcept;

        ~Http();

        static std::unique_ptr<Http> New();

        void SetHeader(const std::string& key, const std::string& value);

        Response Get(const std::string& url);
        Response Post(const std::string& url, const nlohmann::json& body);
        Response PostRaw(const std::string& url, const uint8_t* data,
                         size_t size);

    private:
        Http();

        Response Perform(const std::string& url, const std::string* post_data,
                         const uint8_t* raw_data = nullptr,
                         size_t raw_size = 0);
        static size_t WriteCallback(char* ptr, size_t size, size_t nmemb,
                                    void* userdata);
        static int ProgressCallback(void* clientp, curl_off_t dltotal,
                                    curl_off_t dlnow, curl_off_t ultotal,
                                    curl_off_t ulnow);

        CURL* curl_;
        std::vector<std::string> headers_;
        bool upload_done_;
    };
}  // namespace http

#endif  // SIEGEDB_CLIENT_HTTP_HTTP_HH_
