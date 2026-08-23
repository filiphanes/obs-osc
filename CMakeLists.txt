cmake_minimum_required(VERSION 3.28...3.30)

find_package(Qt6 REQUIRED Widgets)

add_library(obs-osc MODULE)
add_library(OBS::osc ALIAS obs-osc)

target_sources(
  obs-osc
  PRIVATE src/plugin.cpp
          src/osc-message.cpp
          src/osc-message.h
          src/osc-net.cpp
          src/osc-net.h
          src/osc-addresses.cpp
          src/osc-addresses.h
          src/osc-util.cpp
          src/osc-util.h
          src/osc-ui-task.cpp
          src/osc-ui-task.h
          src/osc-server.cpp
          src/osc-server.h
          src/osc-plugin.h
          src/osc-controls.cpp
          src/osc-objects.cpp
          src/osc-objects.h
          src/osc-feedback.cpp
          src/osc-subscribe.cpp
          src/osc-subscribe.h
          src/osc-ws.cpp
          src/osc-ws.h
          src/osc-query.cpp
          src/osc-query.h
          src/osc-query-schema.cpp
          src/osc-query-schema.h
          src/osc-query-http.cpp
          src/osc-query-http.h
          src/osc-query-bonjour.cpp
          src/osc-query-bonjour.h
          src/osc-settings.cpp
          src/osc-settings.h)

target_link_libraries(obs-osc PRIVATE OBS::libobs OBS::frontend-api Qt::Widgets)

target_compile_features(obs-osc PRIVATE cxx_std_17)

if(OS_WINDOWS)
  target_link_libraries(obs-osc PRIVATE ws2_32)
  configure_file(cmake/windows/obs-module.rc.in obs-osc.rc)
  target_sources(obs-osc PRIVATE obs-osc.rc)
endif()

set_target_properties_obs(obs-osc PROPERTIES FOLDER plugins PREFIX "" AUTOMOC ON)
