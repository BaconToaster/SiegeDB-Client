#include "siegedb/siegedb.hh"

// clang-format off
#include <Windows.h>
#include <TlHelp32.h>
// clang-format on
#include <handleapi.h>
#include <memoryapi.h>
#include <processthreadsapi.h>
#include <winnt.h>
#include <zlib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "siegedb/api.hh"

#undef min

namespace siegedb {
    std::unique_ptr<SiegeDB> SiegeDB::New(const std::string& api_url,
                                          const std::string& api_token) {
        std::unique_ptr<SiegeDB> siegedb(new SiegeDB());
        siegedb->api_ = Api::New(api_url, api_token);
        if (!siegedb->api_) {
            return nullptr;
        }
        return siegedb;
    }

    bool SiegeDB::Attach(const std::string& process_name) {
        pid_ = GetProcessId(process_name);
        if (!pid_) {
            printf("[siegedb::SiegeDB::Attach] process not found\n");
            return false;
        }
        image_base_ = GetModuleBase(process_name);
        if (!image_base_) {
            printf("[siegedb::SiegeDB::Attach] image base not found\n");
            return false;
        }
        h_proc_ =
            OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, 0, pid_);
        if (h_proc_ == INVALID_HANDLE_VALUE) {
            printf("[siegedb::SiegeDB::Attach] failed to open process\n");
            return false;
        }
        return true;
    }

    nlohmann::json SiegeDB::GetOffsets(const std::string& query) {
        if (!h_proc_) {
            printf(
                "[siegedb::SiegeDB::GetOffsets] not attached to a process, "
                "call SiegeDB::Attach first\n");
            return nullptr;
        }
        const auto nt = GetNtHeaders();
        const uint32_t timestamp = nt.FileHeader.TimeDateStamp;
        if (!timestamp) {
            printf("[siegedb::SiegeDB::GetOffsets] TimeDateStamp is null\n");
            return nullptr;
        }
        auto response = api_->GetOffsets(timestamp, query);
        switch (response.type) {
            case Api::OffsetsResponse::Type::SUCCESS:
                return *response.data;
            case Api::OffsetsResponse::Type::NOT_FOUND: {
                if (!response.upload_token) {
                    return nullptr;
                }
                std::vector<uint8_t> dump;
                if (!ReadSections(dump)) {
                    printf(
                        "[siegedb::SiegeDB::GetOffsets] failed to read "
                        "process sections\n");
                    return nullptr;
                }
                std::vector<uint8_t> compressed;
                if (!Compress(dump, compressed)) {
                    printf(
                        "[siegedb::SiegeDB::GetOffsets] failed to compress "
                        "dump\n");
                    return nullptr;
                }
                printf(
                    "[siegedb::SiegeDB::GetOffsets] compressed %.1f MB -> "
                    "%.1f MB\n",
                    static_cast<double>(dump.size()) / (1024.0 * 1024.0),
                    static_cast<double>(compressed.size()) / (1024.0 * 1024.0));
                dump.clear();
                dump.shrink_to_fit();

                constexpr size_t kMaxSingleUpload = 50ULL * 1024 * 1024;
                std::string job_id;

                if (compressed.size() <= kMaxSingleUpload) {
                    // Single-chunk upload (existing path)
                    if (!api_->Upload(*response.upload_token, compressed.data(),
                                      compressed.size(), job_id)) {
                        return nullptr;
                    }
                } else {
                    // Chunked parallel upload
                    constexpr size_t kMaxChunkSize = 50ULL * 1024 * 1024;
                    uint32_t chunk_count = static_cast<uint32_t>(
                        (compressed.size() + kMaxChunkSize - 1) /
                        kMaxChunkSize);
                    size_t chunk_size =
                        (compressed.size() + chunk_count - 1) / chunk_count;

                    printf(
                        "[upload] splitting into %u chunks of ~%.1f MB\n",
                        chunk_count,
                        chunk_size / (1024.0 * 1024.0));

                    auto upload_id =
                        api_->InitUpload(*response.upload_token, chunk_count);
                    if (!upload_id) {
                        printf(
                            "[siegedb::SiegeDB::GetOffsets] "
                            "InitUpload failed\n");
                        return nullptr;
                    }
                    printf("[upload] upload_id: %s\n", upload_id->c_str());

                    std::atomic<bool> any_failed{false};
                    std::vector<std::thread> threads;
                    std::string auth_header = api_->GetAuthHeader();
                    std::string base_url = api_->GetUrl();
                    std::string captured_job_id;
                    std::mutex job_id_mutex;

                    for (uint32_t i = 0; i < chunk_count; i++) {
                        size_t offset = static_cast<size_t>(i) * chunk_size;
                        size_t chunk_sz = std::min(
                            chunk_size, compressed.size() - offset);
                        const uint8_t* chunk_ptr = compressed.data() + offset;

                        threads.emplace_back([&any_failed, &auth_header,
                                              &base_url, chunk_ptr, chunk_sz, i,
                                              &upload_id, &captured_job_id,
                                              &job_id_mutex]() {
                            auto http = http::Http::New();
                            if (!http) {
                                any_failed = true;
                                return;
                            }
                            // Parse "Authorization: Bearer <token>"
                            auto colon = auth_header.find(": ");
                            if (colon != std::string::npos) {
                                http->SetHeader(auth_header.substr(0, colon),
                                                auth_header.substr(colon + 2));
                            }
                            std::string url = base_url + "/upload/" +
                                              *upload_id + "/" +
                                              std::to_string(i);
                            auto res =
                                http->PostRaw(url, chunk_ptr, chunk_sz, false);
                            if (!res.error.empty() ||
                                (res.status_code != 200 &&
                                 res.status_code != 202)) {
                                printf(
                                    "\n[upload] chunk %u failed: "
                                    "%s\n",
                                    i,
                                    res.error.empty() ? "unexpected status"
                                                      : res.error.c_str());
                                any_failed = true;
                            } else if (res.body.contains("job_id")) {
                                std::lock_guard<std::mutex> lock(job_id_mutex);
                                captured_job_id =
                                    res.body.value("job_id", "");
                            }
                        });
                    }

                    for (auto& t : threads) {
                        t.join();
                    }

                    if (any_failed) {
                        printf(
                            "[siegedb::SiegeDB::GetOffsets] "
                            "chunked upload failed\n");
                        return nullptr;
                    }

                    job_id = captured_job_id;
                }

                nlohmann::json result;
                if (PollUntilDone(job_id, timestamp, query, result)) {
                    return result;
                }
                return nullptr;
            }
            case Api::OffsetsResponse::Type::WAIT: {
                nlohmann::json result;
                if (PollUntilDone("", timestamp, query, result)) {
                    return result;
                }
                return nullptr;
            }
            case Api::OffsetsResponse::Type::SEND_DATA: {
                std::vector<uint8_t> regions;
                if (!ReadHeapRegions(response.min_size, response.max_size,
                                     regions)) {
                    printf(
                        "[siegedb::SiegeDB::GetOffsets] failed to read heap "
                        "regions\n");
                    return nullptr;
                }
                std::vector<uint8_t> compressed;
                if (!Compress(regions, compressed)) {
                    return nullptr;
                }
                regions.clear();
                regions.shrink_to_fit();
                std::string new_job_id;
                if (!api_->UploadMemory(response.job_id, compressed.data(),
                                        compressed.size(), new_job_id)) {
                    return nullptr;
                }
                nlohmann::json result;
                if (PollUntilDone(new_job_id, timestamp, query, result)) {
                    return result;
                }
                return nullptr;
            }
            default:
                return nullptr;
        }
    }

    bool SiegeDB::PollUntilDone(const std::string& job_id, uint32_t timestamp,
                                const std::string& query,
                                nlohmann::json& result) {
        // If no job_id, poll via GetOffsets until complete
        if (job_id.empty()) {
            for (int i = 0; i < 120; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                auto response = api_->GetOffsets(timestamp, query);
                printf("[poll] attempt %d, response type: %d\n", i + 1,
                       static_cast<int>(response.type));
                switch (response.type) {
                    case Api::OffsetsResponse::Type::SUCCESS:
                        result = *response.data;
                        return true;
                    case Api::OffsetsResponse::Type::WAIT:
                        break;  // keep polling
                    case Api::OffsetsResponse::Type::SEND_DATA: {
                        std::vector<uint8_t> regions;
                        if (!ReadHeapRegions(response.min_size,
                                             response.max_size, regions)) {
                            return false;
                        }
                        std::vector<uint8_t> compressed;
                        if (!Compress(regions, compressed)) {
                            return false;
                        }
                        regions.clear();
                        regions.shrink_to_fit();
                        std::string new_job_id;
                        if (!api_->UploadMemory(
                                response.job_id, compressed.data(),
                                compressed.size(), new_job_id)) {
                            return false;
                        }
                        // Switch to job-based polling
                        return PollUntilDone(new_job_id, timestamp, query,
                                             result);
                    }
                    default:
                        printf("[poll] unexpected response type %d, giving up\n",
                               static_cast<int>(response.type));
                        return false;
                }
            }
            printf(
                "[siegedb::SiegeDB::PollUntilDone] timed out waiting for "
                "analysis\n");
            return false;
        }

        std::string current_job = job_id;
        for (int i = 0; i < 120; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            auto status = api_->GetStatus(current_job);
            switch (status.status) {
                case Api::StatusResponse::Status::COMPLETE: {
                    auto response = api_->GetOffsets(timestamp, query);
                    if (response.type == Api::OffsetsResponse::Type::SUCCESS) {
                        result = *response.data;
                        return true;
                    }
                    return false;
                }
                case Api::StatusResponse::Status::AWAITING_DATA: {
                    std::vector<uint8_t> regions;
                    if (!ReadHeapRegions(status.min_size, status.max_size,
                                         regions)) {
                        printf(
                            "[siegedb::SiegeDB::PollUntilDone] failed to read "
                            "heap regions\n");
                        return false;
                    }
                    std::vector<uint8_t> compressed;
                    if (!Compress(regions, compressed)) {
                        return false;
                    }
                    regions.clear();
                    regions.shrink_to_fit();
                    std::string new_job_id;
                    if (!api_->UploadMemory(current_job, compressed.data(),
                                            compressed.size(), new_job_id)) {
                        return false;
                    }
                    current_job = new_job_id;
                    break;
                }
                case Api::StatusResponse::Status::FAILED:
                    printf(
                        "[siegedb::SiegeDB::PollUntilDone] analysis failed: "
                        "%s\n",
                        status.error_message.c_str());
                    return false;
                case Api::StatusResponse::Status::PENDING:
                case Api::StatusResponse::Status::PROCESSING:
                    break;
                default:
                    return false;
            }
        }
        printf("[siegedb::SiegeDB::PollUntilDone] timed out waiting for job\n");
        return false;
    }

    SiegeDB::SiegeDB()
        : api_(nullptr), pid_(0), h_proc_(nullptr), image_base_(0) {}

    uint32_t SiegeDB::GetProcessId(const std::string& process_name) {
        const std::wstring process_name_w(process_name.begin(),
                                          process_name.end());
        HANDLE process_snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (process_snap == INVALID_HANDLE_VALUE) {
            return 0;
        }
        PROCESSENTRY32W process_info{};
        process_info.dwSize = sizeof(process_info);
        if (Process32FirstW(process_snap, &process_info)) {
            do {
                if (process_name_w == process_info.szExeFile) {
                    CloseHandle(process_snap);
                    return process_info.th32ProcessID;
                }
            } while (Process32NextW(process_snap, &process_info));
        }
        CloseHandle(process_snap);
        return 0;
    }

    uint64_t SiegeDB::GetModuleBase(const std::string& module_name) {
        const std::wstring module_name_w(module_name.begin(),
                                         module_name.end());
        HANDLE module_snap = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_);
        MODULEENTRY32W module_info{};
        module_info.dwSize = sizeof(module_info);
        if (Module32FirstW(module_snap, &module_info)) {
            do {
                if (module_name_w == module_info.szModule) {
                    CloseHandle(module_snap);
                    return reinterpret_cast<uint64_t>(module_info.modBaseAddr);
                }
            } while (Module32NextW(module_snap, &module_info));
        }
        CloseHandle(module_snap);
        return 0;
    }

    IMAGE_NT_HEADERS64 SiegeDB::GetNtHeaders() {
        IMAGE_DOS_HEADER dos_header;
        ReadProcessMemory(h_proc_, reinterpret_cast<void*>(image_base_),
                          &dos_header, sizeof(dos_header), nullptr);
        if (dos_header.e_magic != IMAGE_DOS_SIGNATURE) {
            return {};
        }
        IMAGE_NT_HEADERS64 nt_header;
        ReadProcessMemory(
            h_proc_, reinterpret_cast<void*>(image_base_ + dos_header.e_lfanew),
            &nt_header, sizeof(nt_header), nullptr);
        if (nt_header.Signature != IMAGE_NT_SIGNATURE) {
            return {};
        }
        return nt_header;
    }

    bool SiegeDB::ReadSections(std::vector<uint8_t>& out) {
        constexpr size_t kHeaderSize = 0x1000;

        // Read PE headers
        std::vector<uint8_t> headers(kHeaderSize);
        if (!ReadProcessMemory(h_proc_, reinterpret_cast<void*>(image_base_),
                               headers.data(), kHeaderSize, nullptr)) {
            printf("[siegedb::SiegeDB::ReadSections] failed to read headers\n");
            return false;
        }

        // Parse DOS header
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(headers.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }

        // Parse NT headers
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(headers.data() +
                                                         dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }

        size_t image_size = nt->OptionalHeader.SizeOfImage;
        if (!image_size) {
            return false;
        }

        // Allocate zero-filled buffer
        out.resize(image_size, 0);

        // Copy headers
        std::memcpy(out.data(), headers.data(), kHeaderSize);

        // Walk section table
        auto* section = IMAGE_FIRST_SECTION(nt);
        uint16_t num_sections = nt->FileHeader.NumberOfSections;

        for (uint16_t i = 0; i < num_sections; i++) {
            char name[9] = {};
            std::memcpy(name, section[i].Name, 8);

            if (std::strcmp(name, ".text") == 0 ||
                std::strcmp(name, ".data") == 0 ||
                std::strcmp(name, ".rdata") == 0 ||
                std::strcmp(name, ".tls") == 0) {
                uint32_t va = section[i].VirtualAddress;
                uint32_t size = section[i].Misc.VirtualSize;

                if (va + size > image_size) {
                    continue;
                }

                ReadProcessMemory(h_proc_,
                                  reinterpret_cast<void*>(image_base_ + va),
                                  out.data() + va, size, nullptr);

                printf("[siegedb::SiegeDB::ReadSections] read %s (%u bytes)\n",
                       name, size);
            }
        }

        return true;
    }

    bool SiegeDB::Compress(const std::vector<uint8_t>& in,
                           std::vector<uint8_t>& out) {
        z_stream strm{};
        if (deflateInit(&strm, Z_BEST_SPEED) != Z_OK) {
            printf("[siegedb::SiegeDB::Compress] deflateInit failed\n");
            return false;
        }

        out.resize(deflateBound(&strm, static_cast<uLong>(in.size())));
        strm.next_out = out.data();
        strm.avail_out = static_cast<uInt>(out.size());

        constexpr size_t kChunk = 4 * 1024 * 1024;  // 4MB input chunks
        size_t offset = 0;
        while (offset < in.size()) {
            size_t remaining = in.size() - offset;
            size_t chunk = std::min(kChunk, remaining);
            strm.next_in = const_cast<Bytef*>(in.data() + offset);
            strm.avail_in = static_cast<uInt>(chunk);
            bool last = (offset + chunk >= in.size());
            deflate(&strm, last ? Z_FINISH : Z_NO_FLUSH);
            offset += chunk;
            printf("\r[compress] %zu / %zu MB (%.0f%%)", offset / (1024 * 1024),
                   in.size() / (1024 * 1024), 100.0 * offset / in.size());
            fflush(stdout);
        }
        printf("\n");
        out.resize(strm.total_out);
        deflateEnd(&strm);
        return true;
    }

    bool SiegeDB::ReadHeapRegions(size_t min_size, size_t max_size,
                                  std::vector<uint8_t>& out) {
        typedef NTSTATUS(NTAPI * QueryVMInfo)(HANDLE, PVOID, uint32_t, PVOID,
                                              SIZE_T, PSIZE_T);
        static QueryVMInfo NtQueryVirtualMemory = (QueryVMInfo)GetProcAddress(
            GetModuleHandleA("ntdll.dll"), "NtQueryVirtualMemory");

        struct Region {
            uint64_t base;
            uint64_t size;
            std::vector<uint8_t> data;
        };
        std::vector<Region> regions;

        uint8_t* address = 0;
        while (true) {
            MEMORY_BASIC_INFORMATION mbi{};
            size_t out_size = 0;
            NTSTATUS status = NtQueryVirtualMemory(h_proc_, address, 0, &mbi,
                                                   sizeof(mbi), &out_size);
            if (status < 0) {
                break;
            }

            if (mbi.Protect == PAGE_READWRITE && mbi.State == MEM_COMMIT &&
                mbi.RegionSize >= min_size && mbi.RegionSize <= max_size) {
                Region region;
                region.base = reinterpret_cast<uint64_t>(mbi.BaseAddress);
                region.size = mbi.RegionSize;
                region.data.resize(mbi.RegionSize);
                SIZE_T bytes_read = 0;
                if (ReadProcessMemory(h_proc_, mbi.BaseAddress,
                                      region.data.data(), mbi.RegionSize,
                                      &bytes_read) &&
                    bytes_read > 0) {
                    region.data.resize(bytes_read);
                    region.size = bytes_read;
                    regions.push_back(std::move(region));
                }
            }
            address = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        }

        if (regions.empty()) {
            printf(
                "[siegedb::SiegeDB::ReadHeapRegions] no matching regions "
                "found\n");
            return false;
        }

        // Build binary format: [uint32_t count] [uint64_t base, uint64_t size,
        // uint8_t data[size]] ...
        size_t total = sizeof(uint32_t);
        for (const auto& r : regions) {
            total += sizeof(uint64_t) + sizeof(uint64_t) + r.data.size();
        }
        out.resize(total);
        uint8_t* ptr = out.data();

        uint32_t count = static_cast<uint32_t>(regions.size());
        std::memcpy(ptr, &count, sizeof(count));
        ptr += sizeof(count);

        for (const auto& r : regions) {
            std::memcpy(ptr, &r.base, sizeof(r.base));
            ptr += sizeof(r.base);
            std::memcpy(ptr, &r.size, sizeof(r.size));
            ptr += sizeof(r.size);
            std::memcpy(ptr, r.data.data(), r.data.size());
            ptr += r.data.size();
        }

        printf("[siegedb::SiegeDB::ReadHeapRegions] collected %u regions\n",
               count);
        return true;
    }
}  // namespace siegedb
