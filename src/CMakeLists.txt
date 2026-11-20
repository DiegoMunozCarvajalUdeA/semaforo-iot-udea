# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

# certificados mTLS del ESP32, generados con pki/generar_certs.sh (ver RED.md)
idf_component_register(SRCS ${app_sources}
                       EMBED_TXTFILES ${CMAKE_SOURCE_DIR}/certs/ca.crt
                                      ${CMAKE_SOURCE_DIR}/certs/esp32.crt
                                      ${CMAKE_SOURCE_DIR}/certs/esp32.key)
