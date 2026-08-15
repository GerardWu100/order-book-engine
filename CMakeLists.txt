cmake_minimum_required(VERSION 3.16)
project(order_book_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Matching engine, shared by the command line tool and the tests.
add_library(order_book
  src/order.cpp
  src/order_book.cpp
)
target_include_directories(order_book PUBLIC src)

# Reads a session file of instructions and prints trades and the book.
add_executable(order_book_sim src/main.cpp)
target_link_libraries(order_book_sim PRIVATE order_book)

enable_testing()
add_executable(order_book_tests tests/test_order_book.cpp)
target_link_libraries(order_book_tests PRIVATE order_book)
add_test(NAME order_book_tests COMMAND order_book_tests)
