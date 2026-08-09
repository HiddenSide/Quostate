# Makefile estandar de plugin de VCV Rack.
# Requiere la variable de entorno RACK_DIR apuntando al SDK de Rack
# (por ejemplo: export RACK_DIR=~/Rack-SDK)

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += presets
RACK_DIR ?= ../..
include $(RACK_DIR)/plugin.mk
