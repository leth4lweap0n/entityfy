cmake_minimum_required(VERSION 3.30)
project(entityfy_package_consumer LANGUAGES CXX)

find_package(entityfy CONFIG REQUIRED)

add_executable(entityfy_package_consumer main.cpp)
target_link_libraries(entityfy_package_consumer PRIVATE
    entityfy::core
    entityfy::db_context
    entityfy::migrate
)
