#pragma once

// SQL Server URI parsing, ODBC connection-string building and native error
// classification. Deliberately free of <sql.h>: the values travel as plain
// C++ types so this header compiles on a machine without an ODBC SDK and the
// rules can be tested without a driver.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace entityfy::detail
{

// The defaults describe a local developer instance; a URI overwrites what it
// carries. A database is never defaulted: parse_sqlserver_uri rejects a URI
// without one, because guessing a database silently targets the wrong schema.
struct sqlserver_uri_options
{
    std::string   host{"127.0.0.1"};
    std::uint16_t port{1433};
    std::string   user{"sa"};
    std::string   password;
    std::string   database;
    bool          encrypt{true};
    bool          trust_server_certificate{true};
    bool          mars{false};
};

[[nodiscard]] inline bool equals_case_insensitive(std::string_view left, std::string_view right) noexcept
{
    if(left.size() != right.size())
    {
        return false;
    }

    for(std::size_t index = 0; index < left.size(); ++index)
    {
        auto first = left[index];
        auto second = right[index];

        if(first >= 'A' && first <= 'Z')
        {
            first = static_cast<char>(first - 'A' + 'a');
        }

        if(second >= 'A' && second <= 'Z')
        {
            second = static_cast<char>(second - 'A' + 'a');
        }

        if(first != second)
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] inline int hex_digit(char value) noexcept
{
    if(value >= '0' && value <= '9')
    {
        return value - '0';
    }

    if(value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }

    if(value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }

    return -1;
}

// Decodes %XX escapes. A '%' that is not followed by two hex digits is kept
// literally: a password typed as `pa%ss` must not turn into a parse error, and
// no ODBC credential needs a different treatment for a stray percent sign.
[[nodiscard]] inline std::string percent_decode(std::string_view input)
{
    std::string output;
    output.reserve(input.size());

    for(std::size_t index = 0; index < input.size(); ++index)
    {
        if(input[index] == '%' && index + 2 < input.size())
        {
            const int high = hex_digit(input[index + 1]);
            const int low = hex_digit(input[index + 2]);

            if(high >= 0 && low >= 0)
            {
                output.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }

        output.push_back(input[index]);
    }

    return output;
}

[[nodiscard]] inline bool parse_sqlserver_flag(std::string_view name, std::string_view value)
{
    if(value == "1" || equals_case_insensitive(value, "true"))
    {
        return true;
    }

    if(value == "0" || equals_case_insensitive(value, "false"))
    {
        return false;
    }

    throw std::invalid_argument{"SQL Server URI option `" + std::string{name} + "` must be 0, 1, true or false"};
}

[[nodiscard]] inline std::uint16_t parse_sqlserver_port(std::string_view text)
{
    if(text.empty())
    {
        throw std::invalid_argument{"SQL Server URI port is empty"};
    }

    unsigned long value = 0;

    for(const char digit: text)
    {
        if(digit < '0' || digit > '9')
        {
            throw std::invalid_argument{"SQL Server URI port must be a number: " + std::string{text}};
        }

        value = value * 10 + static_cast<unsigned long>(digit - '0');

        if(value > 65535)
        {
            throw std::invalid_argument{"SQL Server URI port must be between 1 and 65535: " + std::string{text}};
        }
    }

    if(value == 0)
    {
        throw std::invalid_argument{"SQL Server URI port must be between 1 and 65535: " + std::string{text}};
    }

    return static_cast<std::uint16_t>(value);
}

inline void apply_sqlserver_query_option(std::string_view pair, sqlserver_uri_options& options)
{
    if(pair.empty())
    {
        return;
    }

    const auto equals = pair.find('=');

    if(equals == std::string_view::npos)
    {
        throw std::invalid_argument{"SQL Server URI option must be written key=value: " + std::string{pair}};
    }

    const auto key = pair.substr(0, equals);
    const auto value = pair.substr(equals + 1);

    if(equals_case_insensitive(key, "mars"))
    {
        options.mars = parse_sqlserver_flag(key, value);
    }
    else if(equals_case_insensitive(key, "encrypt"))
    {
        options.encrypt = parse_sqlserver_flag(key, value);
    }
    else if(equals_case_insensitive(key, "trust_server_certificate"))
    {
        options.trust_server_certificate = parse_sqlserver_flag(key, value);
    }

    // Unknown keys are ignored on purpose: a URI written for a newer release
    // keeps working instead of failing on an option this build does not know.
}

// sqlserver://user:password@host:port/database?mars=1&encrypt=0
// mssql:// is accepted as the same scheme. The user, password and database are
// percent-decoded; the query string begins at the first '?', so a literal '?'
// in the credentials has to be percent-encoded.
[[nodiscard]] inline sqlserver_uri_options parse_sqlserver_uri(std::string_view uri)
{
    constexpr std::string_view sqlserver_prefix = "sqlserver://";
    constexpr std::string_view mssql_prefix = "mssql://";

    std::string_view rest;

    if(uri.starts_with(sqlserver_prefix))
    {
        rest = uri.substr(sqlserver_prefix.size());
    }
    else if(uri.starts_with(mssql_prefix))
    {
        rest = uri.substr(mssql_prefix.size());
    }
    else
    {
        throw std::invalid_argument{"SQL Server connection string must start with sqlserver:// or mssql://"};
    }

    sqlserver_uri_options options;
    std::string_view       query;

    if(const auto question = rest.find('?'); question != std::string_view::npos)
    {
        query = rest.substr(question + 1);
        rest = rest.substr(0, question);
    }

    if(const auto slash = rest.find('/'); slash != std::string_view::npos)
    {
        options.database = percent_decode(rest.substr(slash + 1));
        rest = rest.substr(0, slash);
    }

    if(const auto at = rest.rfind('@'); at != std::string_view::npos)
    {
        const auto credentials = rest.substr(0, at);
        rest = rest.substr(at + 1);

        if(const auto colon = credentials.find(':'); colon != std::string_view::npos)
        {
            options.user = percent_decode(credentials.substr(0, colon));
            options.password = percent_decode(credentials.substr(colon + 1));
        }
        else
        {
            options.user = percent_decode(credentials);
        }
    }

    if(const auto colon = rest.rfind(':'); colon != std::string_view::npos)
    {
        if(const auto parsed_host = rest.substr(0, colon); !parsed_host.empty())
        {
            options.host = std::string{parsed_host};
        }

        options.port = parse_sqlserver_port(rest.substr(colon + 1));
    }
    else if(!rest.empty())
    {
        options.host = std::string{rest};
    }

    if(options.database.empty())
    {
        throw std::invalid_argument{"SQL Server URI must include a database"};
    }

    // UID, PWD and DATABASE travel as braced ODBC values, so a ';' inside them
    // cannot add a keyword. The host has no such escape and is used verbatim in
    // SERVER=host,port, so a separator in it is a rejected URI rather than a
    // silently injected connection-string option.
    if(options.host.find_first_of(";,{}") != std::string::npos)
    {
        throw std::invalid_argument{
            "SQL Server URI host must not contain ';', ',', '{' or '}': the host is used verbatim in the ODBC "
            "connection string"
        };
    }

    while(!query.empty())
    {
        const auto separator = query.find('&');
        apply_sqlserver_query_option(query.substr(0, separator), options);

        if(separator == std::string_view::npos)
        {
            break;
        }

        query = query.substr(separator + 1);
    }

    return options;
}

// One ODBC braced value: the braces keep ';' and '=' inside the value literal,
// and every '}' is doubled so the closing brace stays unambiguous.
[[nodiscard]] inline std::string odbc_braced_value(std::string_view value)
{
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('{');

    for(const char current: value)
    {
        output.push_back(current);

        if(current == '}')
        {
            output.push_back('}');
        }
    }

    output.push_back('}');
    return output;
}

// Driver 18 is the first driver Microsoft ships for every platform entityfy
// supports; the connection string names it explicitly so a machine with several
// installed drivers cannot pick a different one.
[[nodiscard]] inline std::string sqlserver_connection_string(const sqlserver_uri_options& options)
{
    std::string output{"DRIVER={ODBC Driver 18 for SQL Server};SERVER="};
    output.append(options.host);
    output.push_back(',');
    output.append(std::to_string(options.port));
    output.append(";UID=");
    output.append(odbc_braced_value(options.user));
    output.append(";PWD=");
    output.append(odbc_braced_value(options.password));
    output.append(";DATABASE=");
    output.append(odbc_braced_value(options.database));
    output.append(options.encrypt ? ";Encrypt=yes" : ";Encrypt=no");
    output.append(options.trust_server_certificate ? ";TrustServerCertificate=yes" : ";TrustServerCertificate=no");

    // MARS is enabled with SQLSetConnectAttrW after the connection exists (see
    // sqlserver_backend), not through MARS_Connection=yes: an attribute the
    // driver rejects then surfaces as a typed connection error naming MARS
    // instead of a generic failed connection string.
    return output;
}

// SQL Server native error numbers that map onto an entityfy error type. The
// numbers are stable driver/server ABI, not strings from the message.
enum class sqlserver_error_kind
{
    none,
    duplicate_key,
    foreign_key,
    not_null,
    check,
    deadlock,
    transient,
    connection,
    authentication,
};

// Takes the native number as a 64-bit value so this header stays free of
// <sql.h>; SQLINTEGER (long on Windows, int on unixODBC) converts implicitly.
[[nodiscard]] constexpr sqlserver_error_kind classify_sqlserver_native(std::int64_t native) noexcept
{
    switch(native)
    {
        case 2601: // duplicate key row in object with unique index
        case 2627: // violation of PRIMARY KEY/UNIQUE constraint
            return sqlserver_error_kind::duplicate_key;

        case 547: // conflicted with the FOREIGN KEY/CHECK constraint
            return sqlserver_error_kind::foreign_key;

        case 515: // cannot insert the value NULL into a NOT NULL column
            return sqlserver_error_kind::not_null;

        case 1205: // chosen as the deadlock victim
            return sqlserver_error_kind::deadlock;

        case 18456: // login failed for user
            return sqlserver_error_kind::authentication;

        case 4060: // cannot open the database requested by the login
            return sqlserver_error_kind::connection;

        // Conditions a retry resolves: lock timeout, Azure SQL's throttling and
        // failover family, and the network errors the driver reports through
        // the statement handle.
        case 1222:
        case 10928:
        case 10929:
        case 40197:
        case 40501:
        case 40613:
        case 49918:
        case 49919:
        case 49920:
        case 10053:
        case 10054:
        case 10060:
            return sqlserver_error_kind::transient;

        default:
            return sqlserver_error_kind::none;
    }
}

} // namespace entityfy::detail
