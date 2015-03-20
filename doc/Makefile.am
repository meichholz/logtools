SUFFIXES = .pod .man

if HAVE_POD2MAN

POD2MAN  = pod2man
PODFLAGS = --center=$(PACKAGE) --release=$(VERSION)

man_MANS = teepee.1 tailfd.8 tailfdx.8

teepee.1: teepee.pod
	$(POD2MAN) $(PODFLAGS) --section=1 $< >$@
tailfd.8: tailfd.pod
	$(POD2MAN) $(PODFLAGS) --section=8 $< >$@
tailfdx.8: tailfdx.pod
	$(POD2MAN) $(PODFLAGS) --section=8 $< >$@

endif
