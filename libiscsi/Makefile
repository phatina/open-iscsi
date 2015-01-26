# This Makefile will work only with GNU make.

OSNAME=$(shell uname -s)
OPTFLAGS ?= -O2 -g
WARNFLAGS ?= -Wall -Wstrict-prototypes
CFLAGS = $(OPTFLAGS) $(WARNFLAGS) -I../include -I../usr \
		-D$(OSNAME) -fPIC -D_GNU_SOURCE -fvisibility=hidden
LIB = libiscsi.so.0
TESTS = tests/test_discovery_sendtargets tests/test_discovery_firmware
TESTS += tests/test_login tests/test_logout tests/test_params
TESTS += tests/test_get_network_config tests/test_get_initiator_name
TESTS += tests/test_set_auth tests/test_get_auth

COMMON_SRCS = sysdeps.o
# sources shared between iscsid, iscsiadm and iscsistart
ISCSI_LIB_SRCS = netlink.o transport.o cxgbi.o be2iscsi.o iscsi_timer.o initiator_common.o iscsi_err.o session_info.o iscsi_util.o io.o auth.o discovery.o login.o log.o md5.o sha1.o iface.o idbm.o sysfs.o iscsi_sysfs.o iscsi_net_util.o iscsid_req.o iser.o uip_mgmt_ipc.o
FW_PARAM_SRCS = fw_entry.o prom_lex.o prom_parse.tab.o fwparam_ppc.o fwparam_sysfs.o

# sources shared with the userspace utils, note we build these separately
# to get PIC versions.
COMMON_OBJS = $(patsubst %.o, common-objs/%.o, $(COMMON_SRCS))
USR_OBJS = $(patsubst %.o, usr-objs/%.o, $(ISCSI_LIB_SRCS) strings.o)
FW_OBJS = $(patsubst %.o, fw-objs/%.o, $(FW_PARAM_SRCS))

# Flags for the tests
tests/% : CFLAGS = $(OPTFLAGS) $(WARNFLAGS) -I.

all: lib tests html

lib: $(LIB)
tests: $(TESTS)

common-objs/%.o: ../utils/sysdeps/%.c
	mkdir -p common-objs
	$(CC) $(CFLAGS) -c $< -o $@

usr-objs/%.o: ../usr/%.c
	mkdir -p usr-objs
	$(CC) $(CFLAGS) -c $< -o $@

fw-objs/%.o: ../utils/fwparam_ibft/%.c
	mkdir -p fw-objs
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(COMMON_OBJS) $(FW_OBJS) $(USR_OBJS) libiscsi.o
	$(CC) $(CFLAGS) -shared -Wl,-soname,$(LIB) $^ -o $@
	ln -s -f $(LIB) libiscsi.so

$(TESTS): $(FW_OBJS) $(COMMON_OBJS) $(USR_OBJS) $(LIB)

html: libiscsi.h libiscsi.doxy
	doxygen libiscsi.doxy

clean:
	rm -rf *.o common-objs usr-objs fw-objs libuip-objs libiscsi.so* \
			.depend *~ html $(TESTS) tests/*~

depend:
	gcc $(CFLAGS) -M `ls *.c` > .depend

-include .depend ../usr/.depend
