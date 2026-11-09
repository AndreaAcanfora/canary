# ESP-IDF component; the host build uses the Makefile at the repository root.
idf_component_register(
    SRCS src/config.c src/ds18b20.c src/form.c src/improv.c src/ohp.c src/probe.c
    INCLUDE_DIRS include)
