#!/usr/bin/make -f

LOCALE		:= en_US.UTF-8
LOCDIR		= $(CURDIR)/$(MOZ_DISTDIR)/.locales

ifeq (1,$(MOZ_WANT_UNIT_TESTS))
ifeq (,$(MOZ_TESTS))
TESTS = check xpcshell-tests jstestbrowser reftest crashtest mochitest
else
TESTS = $(MOZ_TESTS)
endif
endif

run-tests: $(addprefix debian/stamp-,$(TESTS))

$(addprefix debian/stamp-,$(TESTS)): debian/stamp-makefile-build

# Required for js/src/trace-tests/sunspider/check-date-format-tofte.js
$(addprefix debian/stamp-,check jstestbrowser): export TZ = :/usr/share/zoneinfo/posix/US/Pacific

$(LOCDIR)/%:
	mkdir -p $(LOCDIR)
	localedef -f $(shell echo $(notdir $@) | cut -d '.' -f 2) -i $(shell echo $(notdir $@) | cut -d '.' -f 1) $@

# Setup locales for tests which need it
$(addprefix debian/stamp-,xpcshell-tests jstestbrowser reftest): $(LOCDIR)/$(LOCALE)
$(addprefix debian/stamp-,xpcshell-tests jstestbrowser reftest): export LOCPATH=$(LOCDIR)
$(addprefix debian/stamp-,xpcshell-tests jstestbrowser reftest): export LC_ALL=$(LOCALE)

# Disable tests that are known to fail
$(addprefix debian/stamp-,xpcshell-tests): debian/stamp-xpcshell-tests-disable

# Tests that need a X server
$(addprefix debian/stamp-,xpcshell-tests jstestbrowser reftest crashtest mochitest): WRAPPER = xvfb-run -a -s "-screen 0 1024x768x24" dbus-launch --exit-with-session

# Run the test!
$(addprefix debian/stamp-,$(TESTS)):
	HOME="$(CURDIR)/$(MOZ_DISTDIR)" \
	$(WRAPPER) $(MAKE) -C $(CURDIR)/$(MOZ_OBJDIR) $(subst debian/stamp-,,$@) || true
	touch $@

debian/stamp-xpcshell-tests-disable:: debian/stamp-makefile-build
	# Hangs without network access
	rm -f $(CURDIR)/$(MOZ_OBJDIR)$(MOZ_MOZDIR)/_tests/xpcshell/toolkit/components/places/tests/unit/test_404630.js

	# Needs GConf to be running. I guess we need to start with dbus-launch to fix this
	rm -f $(CURDIR)/$(MOZ_OBJDIR)$(MOZ_MOZDIR)/_tests/xpcshell/browser/components/shell/test/unit/test_421977.js
	rm -f $(CURDIR)/$(MOZ_OBJDIR)$(MOZ_MOZDIR)/_tests/xpcshell/uriloader/exthandler/tests/unit/test_handlerService.js
	touch $@
