SUBDIRS += hw/ipmi

IPMI_OBJS  = ipmi-rtc.o ipmi-power.o ipmi-opal.o
IPMI = hw/ipmi/built-in.o
$(IPMI): $(IPMI_OBJS:%=hw/ipmi/%)