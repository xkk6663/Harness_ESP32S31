import serial, time, sys

s = serial.Serial('COM9', 115200, timeout=1)
# idf.py flash 结束后板子自动复位，直接读串口即可

end = time.time() + 12
buf = []
while time.time() < end:
    line = s.readline()
    if line:
        txt = line.decode('utf-8', 'replace').rstrip()
        print(txt, flush=True)
        buf.append(txt)
s.close()
print("\n===== CAPTURE END =====")
