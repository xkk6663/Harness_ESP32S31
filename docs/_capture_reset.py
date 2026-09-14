import serial, time, sys

s = serial.Serial('COM9', 115200, timeout=1)
# reset via RTS (EN)
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)
time.sleep(0.1)
s.reset_input_buffer()

end = time.time() + 20
kw = sys.argv[1] if len(sys.argv) > 1 else None
while time.time() < end:
    line = s.readline()
    if line:
        txt = line.decode('utf-8', 'replace').rstrip()
        if not kw or kw in txt:
            print(txt, flush=True)
s.close()
print("===== CAPTURE END =====")
