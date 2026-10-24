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

# Refresh icons
refresh-icons:
	@$(MAKE) del-icons
	@$(MAKE) icons

# Icon deletion
del-icons:
	@echo "Removing old icons..."
	cd src/icon && rm -f *.c *.h *.lst
	@echo "Cleaned!"

# Create icons from pngs
icons:
	@echo "Creating icons..."
	cd src/icon && convimg -i convimg.yaml
	@echo "Done!"

all: refresh-icons
