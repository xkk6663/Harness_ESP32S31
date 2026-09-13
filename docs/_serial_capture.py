import serial, time, sys

s = serial.Serial('COM9', 115200, timeout=1)
# ESP32 自动下载电路：RTS -> EN，拉低再拉高触发硬复位
s.setRTS(True)
time.sleep(0.15)
s.setRTS(False)

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
