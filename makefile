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

# Create the icons
icons:
	@echo "Removing old icons..."
	cd src/icon && rm *.c && rm *.h && rm *.lst
	@echo "Cleaned!"
	@echo "Creating icons..."
	cd src/icon && convimg -i convimg.yaml
	@echo "Done!"
	
all: icons
