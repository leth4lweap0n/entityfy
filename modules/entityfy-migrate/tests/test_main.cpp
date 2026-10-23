#include "harness.hpp"

#include <exception>
#include <iostream>

entityfy_test::status test_schema();
entityfy_test::status test_diff();
entityfy_test::status test_json();
entityfy_test::status test_sqlite();
entityfy_test::status test_engine();
entityfy_test::status test_scaffold();
entityfy_test::status test_postgres();
entityfy_test::status test_history_table();
entityfy_test::status test_rename_table();
entityfy_test::status test_owned_model();

int main()
{
    std::cout << std::unitbuf;

    struct entry
    {
        const char* name;
        entityfy_test::status (*run)();
    };

    const entry entries[] = {
        {"schema", test_schema},
        {"diff", test_diff},
        {"json", test_json},
        {"sqlite", test_sqlite},
        {"engine", test_engine},
        {"scaffold", test_scaffold},
        {"postgres", test_postgres},
        {"history_table", test_history_table},
        {"rename_table", test_rename_table},
        {"owned_model", test_owned_model},
    };

    int passed = 0;
    int skipped = 0;
    int failed = 0;

    for(const auto& current: entries)
    {
        try
        {
            const auto result = current.run();
            if(result == entityfy_test::status::skipped)
            {
                std::cout << "[skip] " << current.name << "\n";
                ++skipped;
            }
            else
            {
                std::cout << "[pass] " << current.name << "\n";
                ++passed;
            }
        }
        catch(const std::exception& error)
        {
            std::cout << "[fail] " << current.name << ": " << error.what() << "\n";
            ++failed;
        }
    }

    std::cout << (failed == 0 ? "all tests passed" : "tests failed") << " (" << passed << " passed, " << skipped
              << " skipped, " << failed << " failed)\n";

    return failed == 0 ? 0 : 1;
}
