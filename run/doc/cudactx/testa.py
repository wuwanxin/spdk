import threading
import pycuda.driver as cuda
# 注意：不导入 pycuda.autoinit！

def worker_thread_correct():
    print(f"\nDEBUG (Worker Thread {threading.get_ident()}): Worker thread started")
    
    worker_context = None
    
    try:
        # 首先初始化CUDA driver
        cuda.init()
        
        # 记录初始状态
        print(f"DEBUG: Initial state - Current context: {cuda.Context.get_current()}")
        
        # ============================================
        # 关键步骤1: 创建Context
        # ============================================
        device = cuda.Device(0)
        worker_context = device.make_context()
        print(f"\nDEBUG: Step 1 - Created context object: {worker_context}")
        
        # 检查make_context是否自动push了Context
        auto_context = cuda.Context.get_current()
        print(f"DEBUG:   Auto-pushed context: {auto_context}")
        print(f"DEBUG:   Same as worker_context? {auto_context == worker_context}")
        
        # ============================================
        # 关键步骤2: 如果有auto-pushed context，先pop它
        # ============================================
        if auto_context is not None:
            print(f"\nDEBUG: Step 2 - Popping auto-pushed context...")
            auto_context.pop()
            print(f"DEBUG:   After pop - Current context: {cuda.Context.get_current()}")
        
        # ============================================
        # 关键步骤3: 现在push我们想要使用的context
        # ============================================
        print(f"\nDEBUG: Step 3 - Pushing worker context...")
        worker_context.push()
        print(f"DEBUG:   After push - Current context: {cuda.Context.get_current()}")
        print(f"DEBUG:   Same as worker_context? {cuda.Context.get_current() == worker_context}")
        
        # ============================================
        # 在这里执行实际工作
        # ============================================
        print(f"\nDEBUG: Step 4 - Doing work in context...")
        import time
        time.sleep(0.1)
        
        # ============================================
        # 关键步骤5: 清理 - pop worker context
        # ============================================
        print(f"\nDEBUG: Step 5 - Popping worker context...")
        worker_context.pop()
        print(f"DEBUG:   After pop - Current context: {cuda.Context.get_current()}")
        
        # ============================================
        # 关键步骤6: 检查并清理任何残留
        # ============================================
        print(f"\nDEBUG: Step 6 - Checking for residual contexts...")
        residual = cuda.Context.get_current()
        if residual is not None:
            print(f"DEBUG:   Found residual context: {residual}")
            print(f"DEBUG:   Popping residual...")
            residual.pop()
        
        # 最终检查
        final = cuda.Context.get_current()
        print(f"DEBUG:   Final context: {final}")
        
        if final is None:
            print(f"DEBUG:   ✅ Context stack is empty")
        else:
            print(f"DEBUG:   ❌ Context stack not empty: {final}")
        
    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
    
    finally:
        print(f"\nDEBUG (Worker Thread {threading.get_ident()}): Worker thread finished")
        return worker_context

# 主测试函数
def main_test():
    print(f"DEBUG (Main Thread {threading.get_ident()}): Test started")
    
    # ============================================
    # 主线程初始化CUDA
    # ============================================
    print(f"\nDEBUG: Main thread initializing CUDA...")
    cuda.init()
    
    # ============================================
    # 主线程创建自己的Context
    # ============================================
    print(f"\nDEBUG: Main thread creating context...")
    device = cuda.Device(0)
    main_context = device.make_context()
    
    # 注意：make_context可能自动push了Context
    auto_context = cuda.Context.get_current()
    print(f"DEBUG: Main thread auto-pushed context: {auto_context}")
    
    # 如果需要，pop掉auto-pushed context
    if auto_context is not None:
        print(f"DEBUG: Main thread popping auto-context...")
        auto_context.pop()
    
    # 现在push主线程的context
    print(f"DEBUG: Main thread pushing main context...")
    main_context.push()
    print(f"DEBUG: Main thread working in context: {cuda.Context.get_current()}")
    
    # ============================================
    # 启动工作线程
    # ============================================
    print(f"\nDEBUG: Starting worker thread...")
    import queue
    result_queue = queue.Queue()
    
    def worker_wrapper():
        context = worker_thread_correct()
        result_queue.put(context)
    
    thread = threading.Thread(target=worker_wrapper)
    thread.start()
    thread.join()
    
    # 获取工作线程的Context对象
    worker_context = result_queue.get()
    
    # ============================================
    # 主线程继续工作
    # ============================================
    print(f"\nDEBUG: Main thread after worker...")
    print(f"DEBUG: Current context: {cuda.Context.get_current()}")
    print(f"DEBUG: Same as main_context? {cuda.Context.get_current() == main_context}")
    
    # ============================================
    # 主线程清理
    # ============================================
    print(f"\nDEBUG: Main thread cleaning up...")
    
    # 首先pop主线程的context
    print(f"DEBUG: Popping main context...")
    main_context.pop()
    
    # 检查是否有残留
    residual = cuda.Context.get_current()
    if residual is not None:
        print(f"DEBUG: Found residual context after pop: {residual}")
        print(f"DEBUG: Popping residual...")
        residual.pop()
    
    # ============================================
    # 尝试detach contexts
    # ============================================
    print(f"\nDEBUG: Detaching contexts...")
    try:
        # detach工作线程的context
        if worker_context is not None:
            print(f"DEBUG: Detaching worker context...")
            worker_context.detach()
        
        # detach主线程的context
        print(f"DEBUG: Detaching main context...")
        main_context.detach()
        
    except Exception as e:
        print(f"DEBUG: Error detaching: {e}")
    
    # ============================================
    # 最终检查
    # ============================================
    print(f"\nDEBUG: Final state check...")
    final_context = cuda.Context.get_current()
    print(f"DEBUG: Final context: {final_context}")
    
    if final_context is None:
        print(f"\n✅ SUCCESS: No context leakage!")
        return True
    else:
        print(f"\n❌ FAIL: Context leakage detected: {final_context}")
        return False

# 运行测试
if __name__ == "__main__":
    success = main_test()
    
    if success:
        print(f"\n🎉 Test completed successfully without any errors!")
    else:
        print(f"\n💥 Test failed!")