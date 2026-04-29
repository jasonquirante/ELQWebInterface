#!/usr/bin/env python3
"""
Upload web files from data/www to ESP32 via HTTP POST.
This is faster than serial-based upload because the web server
is available immediately after bootup.
"""

import os
import sys
import requests
import json
from pathlib import Path

def upload_files(host_url, source_dir):
    """Upload all files from source_dir to the device via HTTP."""
    
    source_path = Path(source_dir)
    if not source_path.exists():
        print(f"❌ Source directory not found: {source_dir}")
        sys.exit(1)
    
    # Get all files recursively
    files_to_upload = []
    for file_path in source_path.rglob('*'):
        if file_path.is_file():
            rel_path = file_path.relative_to(source_path)
            files_to_upload.append((file_path, f"/www/{rel_path}"))
    
    if not files_to_upload:
        print(f"❌ No files found in {source_dir}")
        sys.exit(1)
    
    print(f"📦 Found {len(files_to_upload)} files to upload")
    print(f"🎯 Target: {host_url}")
    print()
    
    failed = []
    succeeded = 0
    
    for local_path, remote_path in sorted(files_to_upload):
        try:
            # Convert Windows paths to forward slashes
            remote_path = remote_path.replace('\\', '/')
            
            with open(local_path, 'rb') as f:
                files = {'file': f}
                data = {'path': remote_path}
                
                response = requests.post(
                    f"{host_url}/api/upload",
                    files=files,
                    data=data,
                    timeout=10
                )
                
                if response.status_code == 200:
                    print(f"✅ {remote_path}")
                    succeeded += 1
                else:
                    error_msg = response.text
                    print(f"❌ {remote_path} - HTTP {response.status_code}")
                    failed.append((remote_path, f"HTTP {response.status_code}: {error_msg}"))
        except Exception as e:
            print(f"❌ {remote_path} - {str(e)}")
            failed.append((remote_path, str(e)))
    
    print()
    print(f"📊 Upload complete: {succeeded} succeeded, {len(failed)} failed")
    
    if failed:
        print("\n⚠️  Failed uploads:")
        for path, error in failed:
            print(f"  • {path}: {error}")
        sys.exit(1)
    else:
        print("\n✨ All files uploaded successfully!")

if __name__ == "__main__":
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.4.1"
    source = sys.argv[2] if len(sys.argv) > 2 else "data/www"
    
    host_url = f"http://{host}" if not host.startswith("http") else host
    
    upload_files(host_url, source)
