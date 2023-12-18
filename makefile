CC = gcc
CFLAGS = -Wall -g

all: home_iot sensor user_console

home_iot: home_iot.c
	$(CC) $(CFLAGS) -pthread -o home_iot home_iot.c

sensor: sensor.c
	$(CC) $(CFLAGS) -o sensor sensor.c

user_console: user_console.c
	$(CC) $(CFLAGS) -pthread -o user_console user_console.c

clean:
	rm -f home_iot sensor user_console