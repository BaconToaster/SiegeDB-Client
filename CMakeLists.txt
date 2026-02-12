cmake_minimum_required(VERSION 3.21)
project(SiegeDB-Client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(${PROJECT_NAME}
    src/main.cc
    src/http/http.cc
    src/siegedb/api.cc
    src/siegedb/siegedb.cc
)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(${PROJECT_NAME} PRIVATE CURL::libcurl nlohmann_json::nlohmann_json ZLIB::ZLIB)
