# ----------------------------
# Makefile Options
# ----------------------------

NAME = FileR
ICON = icon.png
DESCRIPTION = "File Manager FileR"
APPLICATION = YES
APPLICATION_DESCRIPTION = "(C) 2026 Peltlas Vulpine"
COMPRESSED = NO
ARCHIVED = YES

APPVAR_PREFIX = "FilR"
APPVAR_SPLIT_SIZE = 65200

CONVBIN = /usr/bin/convbin

CFLAGS = -Wall -Wextra -Oz
CXXFLAGS = -Wall -Wextra -Oz

# ----------------------------

include $(shell cedev-config --makefile)

.PHONY: appvars installer install-kit clean-install all-clean refresh-icons del-icons icons

appvars:
	@echo "Creating the app..."
	@$(MAKE)
	@echo "App created!"
	@echo "Splitting the app into AppVars..."
	$(CONVBIN) --iformat 8ek --input bin/$(NAME).8ek --oformat 8xv-split --maxvarsize $(APPVAR_SPLIT_SIZE) --output bin/$(APPVAR_PREFIX).8xv --name $(APPVAR_PREFIX)
	@echo "Split!"

installer:
	@$(MAKE) -C installer
	@echo "Bringing the installer over to \`bin\`"
	@mv installer/bin/AINSTALL.8xp bin/AINSTALL.8xp

install-kit: refresh-icons
	@$(MAKE) appvars
	@$(MAKE) installer
	@echo "Moving debug-related files over to their own folder..."
	@mkdir -p bin/debug
	@mv bin/*.* bin/debug
	@mv bin/debug/*.8xp bin/debug/*.8xv bin
	@echo "Finished!"
	@echo "You should be ready to install FileR on your calculator now!"

clean-install:
	@cd installer && make clean

all-clean:
	cd installer && $(MAKE) clean
	@$(MAKE) clean

refresh-icons:
	@$(MAKE) del-icons
	@$(MAKE) icons

del-icons:
	@echo "Removing old icons..."
	@cd src/icon && rm -f *.c *.h *.lst
	@echo "Cleaned!"

icons:
	@echo "Creating icons..."
	@cd src/icon && convimg -i convimg.yaml
	@echo "Done!"

all: install-kit
