#include <entityfy/client.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace entityfy
{
namespace
{

[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
}

} // namespace

client::client(std::string_view uri)
{
    if(starts_with(uri, "sqlite://"))
    {
#if defined(ENTITYFY_ENABLE_SQLITE)
        constexpr std::string_view prefix = "sqlite://";
        std::string path{uri.substr(prefix.size())};

        if(path.empty())
        {
            throw std::invalid_argument{"sqlite URI must include a database path, for example sqlite://app.db or sqlite://:memory:"};
        }

        backend_ = detail::make_sqlite_backend(std::move(path));
        return;
#else
        throw std::invalid_argument{"SQLite backend was not enabled when entityfy was built"};
#endif
    }

    if(starts_with(uri, "postgres://") || starts_with(uri, "postgresql://"))
    {
#if defined(ENTITYFY_ENABLE_POSTGRESQL)
        backend_ = detail::make_postgresql_backend(std::string{uri});
        return;
#else
        throw std::invalid_argument{"PostgreSQL backend was not enabled when entityfy was built"};
#endif
    }

    if(starts_with(uri, "mysql://"))
    {
#if defined(ENTITYFY_ENABLE_MYSQL)
        backend_ = detail::make_mysql_backend(std::string{uri});
        return;
#else
        throw std::invalid_argument{"MySQL backend was not enabled when entityfy was built"};
#endif
    }

    throw std::invalid_argument{
        "unsupported entityfy ORM URI. Expected sqlite://, postgres://, postgresql://, or mysql://"
    };
}

client::~client() = default;

client::client(client&&) noexcept = default;

client& client::operator=(client&&) noexcept = default;

} // namespace entityfy

