AM_CFLAGS=-DSYSCONFDIR=\""$(sysconfdir)/@PACKAGE@/"\"
bin_PROGRAMS = tbfs_tracker
tbfs_tracker_SOURCES = log.c
tbfs_tracker_SOURCES += conf.c
tbfs_tracker_SOURCES += libevent_utils.c
tbfs_tracker_SOURCES += sys_utils.c
tbfs_tracker_SOURCES += string_utils.c
tbfs_tracker_SOURCES += main.c

tbfs_tracker_CFLAGS = $(AM_CFLAGS) $(DEPS_CFLAGS) $(LIBEVENT_OPENSSL_CFLAGS) $(SSL_CFLAGS)
tbfs_tracker_LDADD = $(AM_LDADD) $(DEPS_LIBS) $(LIBEVENT_OPENSSL_LIBS) $(SSL_LIBS)
