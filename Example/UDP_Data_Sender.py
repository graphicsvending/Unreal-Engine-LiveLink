import socket

UDP_IP = "127.0.0.1"
UDP_PORT = 54321

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
BUFFER_SIZE = 1024

def udp_data_sender():
    MESSAGE = '{"SubjectName":{"UserData":[0,0,0,0,0,0,0,0,0,0]}}'
    BUFFER = bytes(MESSAGE, "utf-8")
    sock.sendto(BUFFER, (UDP_IP, UDP_PORT))

while True:
    udp_data_sender()