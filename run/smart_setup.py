import socket
import json
import time

SOCK_PATH = "/var/tmp/spdk.sock"
TARGET_IP = "192.168.1.151" 
MY_HOST_NQN = "nqn.2025-01.io.test:client1"

# 我们一定要 4MB (4194304) 的大门，但传送带(Unit Size)可以协商
TARGET_MAX_IO = 4194304 
# 备选 Unit 列表: 32K, 64K, 8K, 4K (128K和16K已知失败)
CANDIDATE_UNIT_SIZES = [32768, 65536, 8192, 4096]

def rpc(method, params=None):
    req = {"jsonrpc": "2.0", "method": method, "id": 1}
    if params: req["params"] = params
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(SOCK_PATH)
        s.sendall(json.dumps(req).encode('utf-8'))
        resp = json.loads(s.recv(4096).decode('utf-8'))
        return resp
    except Exception as e:
        return {"error": {"message": str(e)}}
    finally:
        s.close()

def setup_transport():
    print(f"--- 正在尝试创建 TCP Transport (MaxIO: 4MB) ---")
    
    for unit_size in CANDIDATE_UNIT_SIZES:
        print(f"🔄 尝试 io_unit_size = {unit_size} ...", end=" ")
        resp = rpc("nvmf_create_transport", {
            "trtype": "TCP",
            "max_io_size": TARGET_MAX_IO,
            "io_unit_size": unit_size
        })
        
        if 'error' not in resp:
            print(f"✅ 成功! (SPDK 接受了 {unit_size})")
            return True
        elif resp['error'].get('code') == -17: # Already exists
             print(f"⚠️ Transport 已存在，跳过。")
             return True
        else:
            print(f"❌ 失败 ({resp['error']['message']})")
    
    print("🚨 所有尝试的 Unit Size 都失败了！")
    return False

if __name__ == "__main__":
    if not setup_transport():
        print("无法建立 Transport，请检查日志。")
        exit(1)

    print(f"--- 配置 Subsystem ---")
    
    # 1. 创建 Subsystem
    rpc("nvmf_create_subsystem", {
        "nqn": "nqn.2016-06.io.spdk:xcode",
        "serial_number": "SPDK001",
        "allow_any_host": False 
    })
    
    # 2. 添加 Host 白名单
    rpc("nvmf_subsystem_add_host", {
        "nqn": "nqn.2016-06.io.spdk:xcode",
        "host": MY_HOST_NQN
    })

    # 3. 挂载 Bdev
    rpc("nvmf_subsystem_add_ns", {
        "nqn": "nqn.2016-06.io.spdk:xcode",
        "namespace": {"bdev_name": "XcoderBdev", "nsid": 1}
    })

    # 4. 监听端口
    rpc("nvmf_subsystem_add_listener", {
        "nqn": "nqn.2016-06.io.spdk:xcode",
        "listen_address": {
            "trtype": "TCP", "adrfam": "IPv4",
            "traddr": TARGET_IP, "trsvcid": "4420"
        }
    })
    
    print("✅ 配置完成！请尝试 Host 连接。")