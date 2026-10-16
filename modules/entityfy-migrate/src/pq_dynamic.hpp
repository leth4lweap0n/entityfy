#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace entityfy::migrate::pq
{

// libpq is loaded dynamically; there is no compile-time libpq dependency.
bool available();
std::string last_error();

class database
{
public:
    explicit database(std::string connection_string);
    ~database();

    database(const database&) = delete;
    database& operator=(const database&) = delete;

    void exec(std::string_view sql);
    [[nodiscard]] std::vector<std::vector<std::string>> query(std::string_view sql);
    [[nodiscard]] std::vector<std::vector<std::string>> query_params(
        std::string_view sql,
        const std::vector<std::string>& params);

    [[nodiscard]] std::string error() const;

private:
    void* handle_ = nullptr;
    void* connection_ = nullptr;
};

} // namespace entityfy::migrate::pq
