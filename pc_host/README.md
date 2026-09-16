# ESP32 Voice Host

`python -m pc_host.main --device 192.168.4.1`

`C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe -m pc_host.main --device 10.241.126.252`


starts the PySide6 host.  The
core protocol and recording server only need the Python standard library;
PySide6, sounddevice and sherpa-onnx are optional runtime integrations.

Install the desktop extras with `pip install -e .[desktop]`.
