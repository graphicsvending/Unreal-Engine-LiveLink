import socket

UDP_IP = "127.0.0.1"
UDP_PORT = 54321

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
BUFFER_SIZE = 1024

SubjectName = "Test01"
XX = 10
YY = 5
ZZ = 180
roll = 0
pitch = 0
yaw = 0


def udp_data_sender():
    MESSAGE = '{"'+SubjectName+'":{"UserData":['+str(XX)+','+str(YY)+','+str(ZZ)+','+str(roll)+','+str(pitch)+','+str(yaw)+',0,0,0,0]}}'
    BUFFER = bytes(MESSAGE, "utf-8")
    sock.sendto(BUFFER, (UDP_IP, UDP_PORT))


while True:

    if ZZ > 180:
        ZZ = 20
    else:
        ZZ = ZZ + 0.0001

    if yaw > 90:
        yaw = -90
    else:
        yaw = yaw + 0.0001

    udp_data_sender()
