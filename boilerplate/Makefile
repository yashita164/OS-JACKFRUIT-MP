obj-m += monitor.o
all: engine cpu_hog memory_hog io_pulse monitor

engine:
	gcc -o engine engine.c -lpthread

cpu_hog:
	gcc -o cpu_hog cpu_hog.c

memory_hog:
	gcc -o memory_hog memory_hog.c

io_pulse:
	gcc -o io_pulse io_pulse.c

monitor:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	rm -f engine cpu_hog memory_hog io_pulse *.o *.ko *.mod.c
