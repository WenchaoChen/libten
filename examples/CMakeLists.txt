cmake_minimum_required(VERSION 2.8)

add_executable(prime_sieve prime_sieve.cc)
target_link_libraries(prime_sieve fw)

add_executable(playground playground.cc)
target_link_libraries(playground fw)

add_executable(file-send file-send.cc)
target_link_libraries(file-send fw)

add_executable(echo-server echo-server.cc)
target_link_libraries(echo-server fw)

add_executable(chat-server chat-server.cc)
target_link_libraries(chat-server fw)