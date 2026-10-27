"""
SPOOFER BS — referencia Python del spoofer (5 capas)
====================================================

Port de referencia del motor C++ (panel/src/spoofer.cpp). Sirve para
auditar la lógica sin compilar, o para automatizar por CLI.

Evidencia base (decomp):
  - 16_spoofer/feasibility: blueprint de 5 capas
  - 16_spoofer/spoofer_automator.py: RUNASINVOKER + rotación de registro
  - 16_spoofer/bloodstrike_hwid_spoofer.py: UDID UniSDK, purga de caches
  - 07_system_footprint/evidence: rutas reales de caches y registro

Uso (como administrador para cobertura completa):
    python spoofer_reference.py inspect
    python spoofer_reference.py apply
    python spoofer_reference.py launch
    python spoofer_reference.py restore backup.json
"""
import ctypes
import json
import os
import random
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

try:
    import winreg
except ImportError:
    print("Este script solo funciona en Windows.")
    sys.exit(1)

TARGET_EXE = r"C:\bloodstrike\Engine\Binaries\Win64\BloodStrike.exe"

TELEMETRY_DOMAINS = [
    "applog.matrix.easebar.com",
    "mgbsdknaeast.matrix.easebar.com",
    "drpf-g83naxx1ena.proxima.nie.easebar.com",
    "nie.easebar.com",
    "gph.easebar.com",
]

CACHE_PATHS = [
    Path(os.environ.get("LOCALAPPDATA", "")) / "NetEase",
    Path(os.environ.get("APPDATA", "")) / "Netease",
    Path(os.environ.get("USERPROFILE", "")) / "AppData" / "LocalLow" / "NetEase",
    Path(os.environ.get("TEMP", "")) / "NetEase",
    Path(os.environ.get("LOCALAPPDATA", "")) / "BloodStrike",
]

HKLM = winreg.HKEY_LOCAL_MACHINE
HKCU = winreg.HKEY_CURRENT_USER

CRYPTO_KEY = r"SOFTWARE\Microsoft\Cryptography"
WINNT_KEY = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion"
HWP_KEY = r"SYSTEM\CurrentControlSet\Control\IDConfigDB\Hardware Profiles\0001"

def log(level, msg):
    print({"i": "[..]   ", "ok": "[OK]   ", "w": "[WARN] ", "e": "[ERROR]"}[level] + msg)

def read_reg(root, key, value):
    try:
        with winreg.OpenKey(root, key, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            return winreg.QueryValueEx(k, value)[0]
    except OSError:
        return None

def inspect():
    footprint = {
        "machine_guid": read_reg(HKLM, CRYPTO_KEY, "MachineGuid"),
        "hw_profile_guid": read_reg(HKLM, HWP_KEY, "HwProfileGuid"),
        "install_date": read_reg(HKLM, WINNT_KEY, "InstallDate"),
        "product_id": read_reg(HKLM, WINNT_KEY, "ProductId"),
        "install_path": read_reg(HKCU, r"Software\Netease\bloodstrike", "installpath"),
    }
    caches = [str(p) for p in CACHE_PATHS if p.exists()]
    print(json.dumps({"registry": footprint, "caches_presentes": caches}, indent=2))
    return footprint

def gen_guid():
    return "{%s}" % str(uuid.uuid4()).upper()

def gen_mac():
    first = random.choice(["02", "06", "0A", "0E"])
    rest = "".join("%02X" % random.randint(0, 255) for _ in range(5))
    return (first + rest).upper()

def gen_volume_serial():
    return "%08X" % random.randint(0x10000000, 0xFFFFFFFE)

def purge_caches():
    for p in CACHE_PATHS:
        if not p.exists():
            continue

        for f in p.rglob("*"):
            if f.is_file():
                try:
                    size = f.stat().st_size
                    with open(f, "r+b") as fh:
                        fh.write(b"\x00" * size)
                        fh.truncate(0)
                except OSError:
                    pass
        try:
            shutil.rmtree(p, ignore_errors=True)
            log("ok", "Cache purgada: %s" % p)
        except OSError as e:
            log("w", "No se pudo purgar %s (%s)" % (p, e))

def rotate_registry():
    def set_str(root, key, value, data):
        try:
            with winreg.CreateKeyEx(root, key, 0, winreg.KEY_ALL_ACCESS | winreg.KEY_WOW64_64KEY) as k:
                winreg.SetValueEx(k, value, 0, winreg.REG_SZ, data)
            return True
        except OSError as e:
            log("w", "Registro %s: %s" % (value, e))
            return False

    ok = set_str(HKLM, CRYPTO_KEY, "MachineGuid", gen_guid().strip("{}"))
    ok &= set_str(HKLM, WINNT_KEY, "ProductId", "%05d-%05d-%05d-%s" % (
        random.randint(0, 99999), random.randint(0, 99999),
        random.randint(0, 99999), random.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ")))
    ok &= set_str(HKLM, WINNT_KEY, "InstallDate", str(random.randint(1640995200, 1767225599)), )
    set_str(HKLM, HWP_KEY, "HwProfileGuid", gen_guid())
    if ok:
        log("ok", "Registro rotado (MachineGuid, ProductId, InstallDate).")

def set_volume_serial(serial_hex):
    """Equivalente Python de Volumeid.exe: reescribe el serial en el boot
    sector de C: (NTFS offset 0x48 / FAT 0x43). Requiere admin."""
    raw = bytes.fromhex(serial_hex)
    h = ctypes.windll.kernel32.CreateFileW(
        r"\\.\C:", 0xC0000000,
        0x3, None, 3, 0, None)
    if h == -1:
        log("e", "No se pudo abrir \\.\\C: (requiere admin).")
        return False
    try:
        sector = ctypes.create_string_buffer(512)
        read = ctypes.c_ulong(0)
        if not ctypes.windll.kernel32.ReadFile(h, sector, 512, ctypes.byref(read), None):
            log("e", "Fallo leyendo boot sector.")
            return False
        b = sector.raw
        is_ntfs = b[3:11] == b"NTFS    "
        is_fat = b[0x36:0x39] == b"FAT" or b[0x52:0x55] == b"FAT"
        if is_ntfs:
            off = 0x48
            new = b[:off] + raw + b[off + 8:]
        elif is_fat:
            off = 0x43
            new = b[:off] + raw[:4] + b[off + 4:]
        else:
            log("e", "C: no parece NTFS ni FAT.")
            return False
        ctypes.windll.kernel32.SetFilePointer(h, 0, None, 0)
        written = ctypes.c_ulong(0)
        if ctypes.windll.kernel32.WriteFile(h, new, 512, ctypes.byref(written), None):
            log("ok", "Volume serial C: -> %s" % serial_hex)
            return True
        log("e", "Fallo escribiendo boot sector.")
        return False
    finally:
        ctypes.windll.kernel32.CloseHandle(h)

def block_neprotect():
    try:
        with winreg.OpenKey(HKLM, r"SYSTEM\CurrentControlSet\Services\NEProtect", 0,
                            winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as k:
            winreg.QueryValueEx(k, "Start")
    except OSError:
        log("w", "NEProtect no está instalado — nada que bloquear.")
        return
    try:
        with winreg.CreateKeyEx(HKLM, r"SYSTEM\CurrentControlSet\Services\NEProtect", 0,
                                winreg.KEY_ALL_ACCESS | winreg.KEY_WOW64_64KEY) as k:
            winreg.SetValueEx(k, "Start", 0, winreg.REG_DWORD, 4)
        log("ok", "NEProtect.sys deshabilitado (Start=4).")
    except OSError as e:
        log("e", "Fallo deshabilitando NEProtect: %s" % e)

def launch_clean():
    env = os.environ.copy()
    env["__COMPAT_LAYER"] = "RUNASINVOKER"
    if Path(TARGET_EXE).exists():
        subprocess.Popen([TARGET_EXE], env=env)
        log("ok", "Juego lanzado en Medium IL: %s" % TARGET_EXE)
    else:
        log("e", "Ejecutable no encontrado: %s" % TARGET_EXE)

def sinkhole_hosts():
    hosts = Path(r"C:\Windows\System32\drivers\etc\hosts")
    text = hosts.read_text(encoding="utf-8", errors="ignore") if hosts.exists() else ""
    if "BS-SPOOFER TELEMETRY" in text:
        log("w", "Sinkhole ya aplicado.")
        return
    block = "\r\n# >>> BS-SPOOFER TELEMETRY >>>\r\n" + "".join(
        "0.0.0.0 %s\r\n" % d for d in TELEMETRY_DOMAINS) + "# <<< BS-SPOOFER TELEMETRY <<<\r\n"
    try:
        import stat
        if hosts.exists():
            os.chmod(str(hosts), stat.S_IWRITE)
        hosts.write_text(text + block, encoding="utf-8")
        log("ok", "Sinkhole aplicado (%d dominios)." % len(TELEMETRY_DOMAINS))
    except OSError as e:
        log("e", "Fallo escribiendo hosts: %s" % e)

def clean_registry_traces():
    for key in (r"Software\Netease",
                r"SOFTWARE\Microsoft\Tracing\BloodStrike_RASAPI32",
                r"SOFTWARE\Microsoft\Tracing\BloodStrike_RASMANCS"):
        try:
            winreg.DeleteKey(HKCU if key.startswith("Software") else HKLM, key)
            log("ok", "Rastro de registro eliminado: %s" % key)
        except OSError:
            pass

def apply_all():
    inspect()
    block_neprotect()
    rotate_registry()
    set_volume_serial(gen_volume_serial())
    purge_caches()
    clean_registry_traces()
    sinkhole_hosts()
    log("ok", "Entorno listo. Lanza con 'launch'.")

def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("inspect", "apply", "launch", "restore"):
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "inspect":
        inspect()
    elif cmd == "apply":
        apply_all()
    elif cmd == "launch":
        launch_clean()
    elif cmd == "restore":
        if len(sys.argv) < 3:
            log("e", "Uso: restore <backup.json>")
            return
        data = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
        for key, value in data.get("registry", {}).items():
            log("i", "restaurar %s = %s" % (key, value))
        log("w", "La restauración completa está implementada en el panel C++ (página Perfiles).")

if __name__ == "__main__":
    main()
