project(lib)

file(GLOB SRCS "*.c")

file(GLOB HEADERS "peer.h" "peer_connection.h" "peer_signaling.h")

add_library(peer
  ${SRCS}
  ${HTTP_SOURCES}
  ${MQTT_SOURCES}
  ${MQTT_SERIALIZER_SOURCES}
)

if(DISABLE_PEER_SIGNALING)
  target_compile_definitions(peer PRIVATE DISABLE_PEER_SIGNALING=1)
else()
  target_include_directories(peer PRIVATE
    ${HTTP_INCLUDE_PUBLIC_DIRS}
    ${MQTT_INCLUDE_PUBLIC_DIRS}
  )
endif()

target_link_libraries(peer ${DEP_LIBS})

set_target_properties(peer PROPERTIES PUBLIC_HEADER "${HEADERS}")

install(TARGETS peer
  ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/
  LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_PREFIX}/include/
)
