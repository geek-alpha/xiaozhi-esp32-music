@echo off
rem Build the ESP-IDF 5.4 env directly: export.bat fails to detect python on this machine
set IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.4
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.4_py3.13_env
set IDF_TOOLS_PATH=C:\Espressif
set PATH=C:\Espressif\python_env\idf5.4_py3.13_env\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\idf-exe\1.0.3;C:\Espressif\tools\ccache\4.10.2\ccache-4.10.2-windows-x86_64;C:\Espressif\tools\esp-rom-elfs\20241011;%PATH%
cd /d D:\AI\tmp_xz_music
%*
