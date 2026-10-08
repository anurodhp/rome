# rome: a small, fast GNUstep terminal (see README.md).
#
# Needs libvterm (0.3.x), FreeType, fontconfig, Xlib with MIT-SHM (Xext)
# and libGL with GLX. Where they are not on the default search paths, pass
# the flags in, e.g.
#   make ADDITIONAL_CPPFLAGS="-I/opt/vterm/include -I/usr/include/freetype2" \
#        ADDITIONAL_LIB_DIRS="-L/opt/vterm/lib"
include $(GNUSTEP_MAKEFILES)/common.make

APP_NAME = Rome
VERSION = 0.1
Rome_OBJC_FILES = main.m RomeView.m RomeTabs.m
Rome_C_FILES = RomeTerm.c RomeFont.c RomeX.c RomeRenderX11.c RomeRenderGL.c
Rome_MAIN_MODEL_FILE =
Rome_APPLICATION_ICON = Rome.png
Rome_RESOURCE_FILES = Rome.png
Rome_LOCALIZED_RESOURCE_FILES =

ADDITIONAL_CFLAGS += -std=gnu99 -O2 -Wall
ADDITIONAL_OBJCFLAGS += -O2 -Wall
ADDITIONAL_GUI_LIBS += -lvterm -lfontconfig -lfreetype -lGL -lXext -lX11

include $(GNUSTEP_MAKEFILES)/application.make
