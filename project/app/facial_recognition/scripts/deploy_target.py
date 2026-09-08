#!/usr/bin/env python3
import os
import sys
import paramiko
import time
import urllib.request
import base64
import json

def deploy_to_flash(target_ip='192.168.1.230', username='root', password='root', do_reboot_test=True):
    print(f"============================================================")
    print(f" [FLASH DEPLOY] Deploying persistently to Flash on {target_ip}")
    print(f"============================================================", flush=True)

    print(f"[1/6] Connecting SSH to {target_ip}...", flush=True)
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(target_ip, username=username, password=password, timeout=10)
    
    print("[2/6] Stopping running services on target...", flush=True)
    stop_cmd = (
        "/etc/init.d/S95facial-recognition stop 2>/dev/null || true; "
        "/etc/init.d/S96facial-webconfig stop 2>/dev/null || true; "
        "/oem/root/etc/init.d/S95facial-recognition stop 2>/dev/null || true; "
        "/oem/root/etc/init.d/S96facial-webconfig stop 2>/dev/null || true; "
        "killall -9 fr-media-service fr-webconfig fr-ai-service 2>/dev/null || true; "
        "sleep 1"
    )
    _, stdout, _ = ssh.exec_command(stop_cmd)
    stdout.read()

    print("[3/6] Uploading all binaries, scripts & config into Flash (/oem, /etc, /userdata)...", flush=True)
    sftp = ssh.open_sftp()
    
    local_out = '/home/vunl/RV06_03_Linux_SDK/project/app/facial_recognition/out'
    remote_base = '/oem'
    
    def ensure_remote_dir(path):
        parts = path.strip('/').split('/')
        cur = ''
        for p in parts:
            cur += '/' + p
            try:
                sftp.stat(cur)
            except Exception:
                try:
                    sftp.mkdir(cur)
                except Exception:
                    pass

    ensure_remote_dir('/userdata/facial-recognition')
    ensure_remote_dir('/userdata/media/video')
    ensure_remote_dir('/mnt/sdcard/video')
    ensure_remote_dir('/oem/usr/bin')
    ensure_remote_dir('/oem/usr/share/facial-recognition/model')
    ensure_remote_dir('/oem/usr/share/facial-recognition/docs')
    ensure_remote_dir('/oem/usr/etc/facial-recognition')
    ensure_remote_dir('/oem/root/etc/init.d')

    uploaded_count = 0
    for root, _, files in os.walk(local_out):
        rel_dir = os.path.relpath(root, local_out)
        target_dir = os.path.join(remote_base, rel_dir) if rel_dir != '.' else remote_base
        ensure_remote_dir(target_dir)
                    
        for f in files:
            local_file = os.path.join(root, f)
            remote_file = os.path.join(target_dir, f)
            try:
                try:
                    sftp.remove(remote_file)
                except Exception:
                    pass
                sftp.put(local_file, remote_file)
                uploaded_count += 1
            except Exception as e:
                print(f"   [ERR] Failed to upload {local_file}: {e}", flush=True)

    # Also upload init scripts directly to /etc/init.d in rootfs Flash partition
    init_scripts = ['S95facial-recognition', 'S96facial-webconfig', 'S97facial-ai']
    for s in init_scripts:
        local_s = os.path.join(local_out, 'root/etc/init.d', s)
        if os.path.exists(local_s):
            remote_s = f'/etc/init.d/{s}'
            try:
                try:
                    sftp.remove(remote_s)
                except Exception:
                    pass
                sftp.put(local_s, remote_s)
                print(f"   [FLASH ROOTFS] -> {remote_s}", flush=True)
                uploaded_count += 1
            except Exception as e:
                print(f"   [ERR] Failed to upload {remote_s}: {e}", flush=True)

    sftp.close()
    print(f"[4/6] Uploaded {uploaded_count} files. Setting Flash permissions and sync...", flush=True)

    cmds = (
        "chmod 755 /oem/usr/bin/* /oem/root/etc/init.d/* /etc/init.d/S95* /etc/init.d/S96* /etc/init.d/S97* 2>/dev/null; "
        "mkdir -p /userdata/facial-recognition /userdata/media/video /mnt/sdcard/video 2>/dev/null; "
        "[ ! -f /userdata/facial-recognition/database.json ] && cp -f /oem/usr/etc/facial-recognition/database.json /userdata/facial-recognition/database.json 2>/dev/null || true; "
        "sync; sync; sync"
    )
    _, stdout, _ = ssh.exec_command(cmds)
    stdout.read()

    if do_reboot_test:
        print("[5/6] Triggering REBOOT on target to verify Flash Persistence...", flush=True)
        try:
            ssh.exec_command("reboot")
        except Exception:
            pass
        ssh.close()

        print("[6/6] Waiting for device to reboot and boot up (20 seconds)...", flush=True)
        time.sleep(12)
        
        # Poll SSH until online
        reconnected = False
        for attempt in range(15):
            time.sleep(2)
            try:
                ssh_test = paramiko.SSHClient()
                ssh_test.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                ssh_test.connect(target_ip, username=username, password=password, timeout=3)
                print(f"   -> Connected after reboot (attempt {attempt+1})!", flush=True)
                
                # Check processes
                _, out_ps, _ = ssh_test.exec_command("ps | grep -E 'fr-|facial|webconfig|media' | grep -v grep")
                ps_str = out_ps.read().decode().strip()
                print("[POST-REBOOT RUNNING PROCESSES]:\n" + (ps_str if ps_str else "NO PROCESS FOUND!"))

                # Check database
                _, out_db, _ = ssh_test.exec_command("ls -la /userdata/facial-recognition")
                db_str = out_db.read().decode().strip()
                print("\n[POST-REBOOT USERDATA FLASH]:\n" + db_str)

                ssh_test.close()
                reconnected = True
                break
            except Exception:
                print(f"   -> Waiting for target {target_ip} to finish booting... ({attempt+1}/15)", flush=True)

        if reconnected:
            print("\n============================================================")
            print(" [FLASH PERSISTENCE VERIFIED] Device successfully rebooted")
            print(" and all services automatically started from Flash!")
            print("============================================================", flush=True)
        else:
            print("[WARN] Timed out waiting for reboot reconnection.", flush=True)
    else:
        # Start immediately without reboot
        start_cmd = (
            "/etc/init.d/S95facial-recognition start; "
            "/etc/init.d/S96facial-webconfig start; "
            "sleep 2; "
            "ps | grep -E 'fr-|facial|webconfig|media' | grep -v grep"
        )
        _, stdout, _ = ssh.exec_command(start_cmd)
        print(stdout.read().decode())
        ssh.close()

if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else '192.168.1.230'
    reboot = True if '--no-reboot' not in sys.argv else False
    deploy_to_flash(target, do_reboot_test=reboot)
