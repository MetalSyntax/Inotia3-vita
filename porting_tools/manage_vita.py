#!/usr/bin/env python3
import os
import sys
import subprocess
from ftplib import FTP, all_errors

VITA_IP = "192.168.3.15"
VITA_PORT = 1337
LOCAL_VPK_PATH = "build/inotia3.vpk"
VITA_DOWNLOADS_DIR = "/ux0:/downloads"
VITA_DATA_DIR = "/ux0:/data"
VITA_LOGS_DIR = "/ux0:/data/inotia3/logs"
BASE_DIR = os.path.dirname(os.path.abspath(__file__))

def print_banner():
    print("====================================================")
    print("         PS VITA DEPLOYMENT & DEBUG TOOL            ")
    print("                   (Inotia 3)                       ")
    print("====================================================")

def disconnect_proton_vpn():
    print("[*] Intentando desconectar Proton VPN...")
    try:
        result = subprocess.run(["protonvpn-cli", "disconnect"], capture_output=True, text=True)
        if result.returncode == 0:
            print("[+] Proton VPN desconectado exitosamente.")
            return True
        else:
            if "not connected" in result.stderr.lower() or "not connected" in result.stdout.lower():
                print("[+] Proton VPN ya estaba desconectado.")
                return True
            print(f"[-] Error de Proton VPN: {result.stderr.strip() or result.stdout.strip()}")
    except FileNotFoundError:
        print("[!] 'protonvpn-cli' no está instalado en el PATH.")
    except Exception as e:
        print(f"[-] Ocurrió un error inesperado al intentar desconectar VPN: {e}")
    return False

def get_local_ip_for_route():
    import socket
    try:
        hostname = socket.gethostname()
        for ip in socket.gethostbyname_ex(hostname)[2]:
            if ip.startswith("192.168.3."):
                return ip
    except Exception:
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((VITA_IP, 9))
        ip = s.getsockname()[0]
        s.close()
        if ip.startswith("192.168.3."):
            return ip
    except Exception:
        pass
    return None

def connect_ftp():
    print(f"[*] Conectando a la PS Vita en {VITA_IP}:{VITA_PORT}...")
    local_ip = get_local_ip_for_route()
    source_addr = (local_ip, 0) if local_ip else None
    if local_ip:
        print(f"[*] Forzando ruta local mediante IP origen física: {local_ip} (Bypasseando VPN)")
    try:
        ftp = FTP()
        ftp.connect(VITA_IP, VITA_PORT, timeout=10, source_address=source_addr)
        ftp.login()
        print("[+] Conexión FTP establecida.")
        return ftp
    except all_errors as e:
        print(f"[-] Error al conectar por FTP a la PS Vita: {e}")
        return None

def create_directory_if_not_exists(ftp, path):
    try:
        ftp.cwd(path)
    except all_errors:
        print(f"[*] El directorio '{path}' no existe. Intentando crearlo...")
        try:
            parts = [p for p in path.split('/') if p]
            current = ""
            for part in parts:
                if ":" in part:
                    current = part
                else:
                    current = f"{current}/{part}"
                try:
                    ftp.mkd(current)
                except all_errors:
                    pass
            ftp.cwd(path)
            print(f"[+] Directorio '{path}' listo.")
        except all_errors as e:
            print(f"[-] No se pudo crear el directorio '{path}': {e}")

def upload_vpk():
    if not os.path.exists(LOCAL_VPK_PATH):
        print(f"[-] Error: No se encontró el archivo VPK local en '{LOCAL_VPK_PATH}'.")
        return

    disconnect_proton_vpn()
    ftp = connect_ftp()
    if not ftp:
        return

    try:
        create_directory_if_not_exists(ftp, VITA_DOWNLOADS_DIR)
        filename = os.path.basename(LOCAL_VPK_PATH)
        dest_file_path = f"{VITA_DOWNLOADS_DIR}/{filename}"

        print(f"[*] Subiendo {LOCAL_VPK_PATH} a {dest_file_path}...")

        with open(LOCAL_VPK_PATH, "rb") as f:
            ftp.storbinary(f"STOR {dest_file_path}", f)

        print(f"[+] Transferencia exitosa! Instala el VPK en tu Vita desde '{VITA_DOWNLOADS_DIR.replace('/ux0:', 'ux0:')}/{filename}'")
    except all_errors as e:
        print(f"[-] Falló la transferencia del VPK: {e}")
    finally:
        try:
            ftp.quit()
        except Exception:
            pass

def download_latest_debug_files():
    disconnect_proton_vpn()
    ftp = connect_ftp()
    if not ftp:
        return

    descargar_dmp = input("¿Deseas descargar el último crash dump (DMP)? (s/n): ").strip().lower()

    if descargar_dmp in ['s', 'y', 'si', 'yes']:
        print("[*] Buscando el último crash dump (.dmp / psp2core) en ux0:/data...")
        latest_dmp = None
        latest_dmp_time = 0
        try:
            ftp.cwd(VITA_DATA_DIR)
            try:
                files = list(ftp.mlsd())
                for name, facts in files:
                    if (name.startswith("psp2core") or name.endswith(".dmp")) and not name.endswith(".tmp"):
                        mtime = facts.get("modify", "0")
                        if mtime > str(latest_dmp_time):
                            latest_dmp_time = int(mtime) if mtime.isdigit() else mtime
                            latest_dmp = name
            except all_errors:
                file_list = []
                ftp.retrlines("LIST", file_list.append)
                valid_files = []
                for line in file_list:
                    parts = line.split()
                    if len(parts) >= 9:
                        name = " ".join(parts[8:])
                        if (name.startswith("psp2core") or name.endswith(".dmp")) and not name.endswith(".tmp"):
                            valid_files.append(name)
                if valid_files:
                    valid_files.sort()
                    latest_dmp = valid_files[-1]

            if latest_dmp:
                project_root = os.path.dirname(BASE_DIR)
                logs_folder = os.path.join(project_root, "logs")
                if not os.path.exists(logs_folder):
                    os.makedirs(logs_folder)
                local_dmp_name = os.path.join(logs_folder, f"Inotia3-{latest_dmp}")
                print(f"[+] Último dump detectado: '{latest_dmp}'. Descargando como '{local_dmp_name}'...")
                with open(local_dmp_name, "wb") as f:
                    ftp.retrbinary(f"RETR {latest_dmp}", f.write)
                print(f"[+] Descargado '{latest_dmp}' en la carpeta logs/.")
            else:
                print("[-] No se encontraron archivos de crash dump en ux0:/data.")
        except all_errors as e:
            print(f"[-] Error al buscar o descargar dump: {e}")
    else:
        print("[*] Omitiendo la descarga del crash dump.")

    print(f"[*] Buscando el último log de Inotia 3 en {VITA_LOGS_DIR}...")
    latest_log = None
    latest_log_time = 0
    try:
        ftp.cwd(VITA_LOGS_DIR)
        try:
            files = list(ftp.mlsd())
            for name, facts in files:
                if name.endswith(".txt") or "log" in name.lower():
                    mtime = facts.get("modify", "0")
                    if mtime > str(latest_log_time):
                        latest_log_time = int(mtime) if mtime.isdigit() else mtime
                        latest_log = name
        except all_errors:
            file_list = []
            ftp.retrlines("LIST", file_list.append)
            valid_logs = []
            for line in file_list:
                parts = line.split()
                if len(parts) >= 9:
                    name = " ".join(parts[8:])
                    if name.endswith(".txt") or "log" in name.lower():
                        valid_logs.append(name)
            if valid_logs:
                valid_logs.sort()
                latest_log = valid_logs[-1]

        if latest_log:
            project_root = os.path.dirname(BASE_DIR)
            logs_folder = os.path.join(project_root, "logs")
            if not os.path.exists(logs_folder):
                os.makedirs(logs_folder)
            local_log_name = os.path.join(logs_folder, f"{latest_log}")
            print(f"[+] Último log detectado: '{latest_log}'. Descargando como '{local_log_name}'...")
            with open(local_log_name, "wb") as f:
                ftp.retrbinary(f"RETR {latest_log}", f.write)
            print(f"[+] Descargado '{latest_log}' en la carpeta logs/.")
        else:
            print(f"[-] No se encontraron archivos de registro (.txt) en {VITA_LOGS_DIR}.")
    except all_errors as e:
        print(f"[-] Error al buscar o descargar logs: {e}")
    finally:
        try:
            ftp.quit()
        except Exception:
            pass

def check_libshacccg():
    """Chequea por FTP si libshacccg.suprx existe y su tamaño en las 2 rutas
    donde vitaGL lo busca. Un tamaño de 0 o muy chico (esperado: ~1-2 MB) o
    directamente "no existe" en ambas indica una instalación corrupta/
    incompleta -- confirmar esto antes de reportar un crash de shaders como
    bug propio del port (ver psvita-porting skill, Fase 4)."""
    disconnect_proton_vpn()
    ftp = connect_ftp()
    if not ftp:
        return

    candidates = [
        "/ur0:/data/libshacccg.suprx",
        "/ur0:/data/external/libshacccg.suprx",
    ]

    try:
        ftp.voidcmd("TYPE I")  # binary mode -- SIZE es poco confiable/rechazado en modo ASCII
        for path in candidates:
            try:
                size = ftp.size(path)
                print(f"[+] {path}: existe, {size} bytes"
                      + ("  <-- sospechosamente chico/vacio!" if not size or size < 100000 else ""))
            except all_errors as e:
                print(f"[-] {path}: no encontrado ({e})")
    finally:
        try:
            ftp.quit()
        except Exception:
            pass

def run_script(folder, script_name, is_python=False):
    script_path = os.path.join(BASE_DIR, folder, script_name)
    if os.path.exists(script_path):
        print(f"[*] Ejecutando {script_name} desde {folder}...")
        try:
            cmd = [sys.executable if is_python else "bash", script_path]
            subprocess.run(cmd, check=True)
            print(f"[+] {script_name} ejecutado exitosamente.")
        except subprocess.CalledProcessError as e:
            print(f"[-] Error al ejecutar {script_name}: {e}")
        except Exception as e:
            print(f"[-] Ocurrió un error inesperado: {e}")
    else:
        print(f"[-] Error: No se encontró el script en '{script_path}'.")

def main():
    while True:
        print_banner()
        print("1. Subir VPK compilado a la PS Vita (ux0:downloads/)")
        print("2. Descargar el último dump (.dmp) y log (.txt) de Inotia 3")
        print("3. Desconectar Proton VPN ahora mismo")
        print("4. Ejecutar clean_macos.sh (build/)")
        print("5. Ejecutar build_and_install.sh (build/)")
        print("6. Ejecutar deploy_and_launch_vita3k.sh (build/)")
        print("7. Ejecutar decompile_all.sh (build/)")
        print("8. Ejecutar run_tests.sh (tests/)")
        print("9. Ejecutar get_dump.sh (misc/)")
        print("10. Chequear libshacccg.suprx (tamano/existencia por FTP)")
        print("11. Salir")
        print("====================================================")
        try:
            opcion = input("Elige una opción (1-11): ").strip()
            print()
            if opcion == "1":
                upload_vpk()
            elif opcion == "2":
                download_latest_debug_files()
            elif opcion == "3":
                disconnect_proton_vpn()
            elif opcion == "4":
                run_script("build", "clean_macos.sh")
            elif opcion == "5":
                run_script("build", "build_and_install.sh")
            elif opcion == "6":
                run_script("build", "deploy_and_launch_vita3k.sh")
            elif opcion == "7":
                run_script("build", "decompile_all.sh")
            elif opcion == "8":
                run_script("tests", "run_tests.sh")
            elif opcion == "9":
                run_script("misc", "get_dump.sh")
            elif opcion == "10":
                check_libshacccg()
            elif opcion == "11":
                print("¡Hasta luego!")
                break
            else:
                print("[-] Opción no válida. Inténtalo de nuevo.")
        except KeyboardInterrupt:
            print("\n¡Hasta luego!")
            break
        print("\nPresiona ENTER para volver al menú principal...")
        input()

if __name__ == "__main__":
    main()
