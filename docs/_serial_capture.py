import serial, time, sys
s = serial.Serial('COM9', 115200, timeout=1)
end = time.time() + 15
while time.time() < end:
    line = s.readline()
    if line:
        print(line.decode('utf-8', 'replace').rstrip())
s.close()
