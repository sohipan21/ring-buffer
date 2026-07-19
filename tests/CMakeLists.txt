add_executable(test_spsc test_spsc.cpp)
target_link_libraries(test_spsc PRIVATE ringbuffer::ringbuffer)
ringbuffer_configure_target(test_spsc)
add_test(NAME spsc COMMAND test_spsc)

add_executable(test_mpmc test_mpmc.cpp)
target_link_libraries(test_mpmc PRIVATE ringbuffer::ringbuffer)
ringbuffer_configure_target(test_mpmc)
add_test(NAME mpmc COMMAND test_mpmc)
