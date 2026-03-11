import threading
import pycuda.driver as cuda
import pycuda.autoinit

def worker_thread_simple():
    print(f"\nDEBUG (Worker Thread {threading.get_ident()}): Worker thread started")
    
    try:
        # 记录初始状态
        print(f"DEBUG: Initial state - Current context: {cuda.Context.get_current()}")
        
        # 创建Context
        device = cuda.Device(0)
        worker_context = device.make_context()
        print(f"\nDEBUG: Created context: {worker_context}")
        
        # 检查并pop auto-pushed context
        auto_context = cuda.Context.get_current()
        if auto_context is not None:
            print(f"DEBUG: Auto-pushed context: {auto_context}")
            print(f"DEBUG: Popping auto-context...")
            auto_context.pop()
        
        # Push worker context
        print(f"DEBUG: Pushing worker context...")
        worker_context.push()
        print(f"DEBUG: Working in context: {cuda.Context.get_current()}")
        
        # 模拟工作
        import time
        time.sleep(0.1)
        
        # Pop worker context
        print(f"DEBUG: Popping worker context...")
        worker_context.pop()
        
        # 清理残留
        residual = cuda.Context.get_current()
        if residual is not None:
            print(f"DEBUG: Cleaning residual: {residual}")
            residual.pop()
        
        print(f"DEBUG: Final context: {cuda.Context.get_current()}")
        
    except Exception as e:
        print(f"ERROR: {e}")
    
    finally:
        print(f"\nDEBUG (Worker Thread {threading.get_ident()}): Worker thread finished")
        # 注意：不返回context，也不调用detach

# 主测试
print(f"DEBUG (Main Thread {threading.get_ident()}): Test started")

# 主线程使用autoinit管理的Context
print(f"DEBUG: Main thread context (managed by autoinit): {cuda.Context.get_current()}")

# 启动工作线程
thread = threading.Thread(target=worker_thread_simple)
thread.start()
thread.join()

print(f"\nDEBUG: Main thread after worker: {cuda.Context.get_current()}")
print(f"\n✅ Test completed! autoinit will handle cleanup.")