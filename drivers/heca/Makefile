ccflags-y += -I${PWD}

heca-y := hutils.o hecatonchire.o transport.o hproc.o mr.o hspace.o  ioctl.o base.o conn.o ops.o pull.o push.o struct.o task.o 

obj-$(CONFIG_HECA) += heca.o

modules clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) $@

