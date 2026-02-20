all:
	gcc -Wall -Wextra -O2 main.c -o zmq_proxy -lzmq -lasound

install:
	systemctl stop linht-zmq-proxy
	cp zmq_proxy /usr/bin
	systemctl start linht-zmq-proxy
