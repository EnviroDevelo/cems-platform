APP_SRC  = $(wildcard app_vmmux/faithful/*_faithful.c) $(wildcard app_vmmux/src/*.c)
APP_INC  = -Iapp_vmmux/include -Iapp_vmmux/faithful
APP_HDRS = $(wildcard app_vmmux/include/*.h app_vmmux/faithful/*.h)
