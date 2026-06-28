file(GLOB_RECURSE all_examples CONFIGURE_DEPENDS *.cpp)

foreach(example ${all_examples})
    get_filename_component(target_name ${example} NAME_WE)
    set(target_name "example_${target_name}")

    add_executable(${target_name} ${example})
    target_link_libraries(${target_name} PRIVATE coronet::coronet)
endforeach()
