import socket
import json

SOCK_PATH = "/var/tmp/spdk.sock"
TARGET_IP = "192.168.1.135" 

# 定义一个合法的 Host NQN
MY_HOST_NQN = "nqn.2025-01.io.test:client1"

def rpc(method, params=None):
    req = {"jsonrpc": "2.0", "method": method, "id": 1}
    if params: req["params"] = params
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(SOCK_PATH)
        s.sendall(json.dumps(req).encode('utf-8'))
        resp = json.loads(s.recv(4096).decode('utf-8'))
        if 'error' in resp:
            print(f"❌ {method}: {resp['error']['message']}")
        else:
            print(f"✅ {method}: OK")
    except Exception as e:
        print(f"❌ {method}: {e}")
    finally:
        s.close()

if __name__ == "__main__":
    print(f"--- Setting up SPDK on {TARGET_IP} ---")
    
    # 1. 创建 Subsystem (先设为 False，完全依赖白名单)
    rpc("nvmf_create_subsystem", {
        "nqn": "nqn.2016-06.io.spdk:xcode",
        "serial_number": "SPDK001",
        "allow_any_host": False 
    })
    
    # 2. 添加合法的 Host NQN
    rpc("nvmf_subsystem_add_host", {
        "nqn": "nqn.2016-06.io.spdk:xcode",
        "host": MY_HOST_NQN  # <--- 使用 nqn.2025... 格式
    })

    # 3. 挂载 Namespace
    rpc("nvmf_subsystem_add_ns", {
        "nqn": "nqn.2016-06.io.spdk:xcode",
        "namespace": {"bdev_name": "XcoderBdev", "nsid": 1}
    })

    # 4. 监听具体 IP
    rpc("nvmf_subsystem_add_listener", {
        "nqn": "nqn.2016-06.io.spdk:xcode",
        "listen_address": {
            "trtype": "TCP", "adrfam": "IPv4",
            "traddr": TARGET_IP, "trsvcid": "4420"
        }
    })