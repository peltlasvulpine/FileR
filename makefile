# ----------------------------
# Makefile Options
# ----------------------------

NAME = FILER
ICON = icon.png
DESCRIPTION = "File Manager FileR"
COMPRESSED = yes

CFLAGS = -Wall -Wextra -Oz
CXXFLAGS = -Wall -Wextra -Oz

# ----------------------------

include $(shell cedev-config --makefile)
