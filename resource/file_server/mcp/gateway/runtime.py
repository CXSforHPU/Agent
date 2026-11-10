"""后台线程上的事件循环。

HTTP 请求由线程池里的工作线程收进来，工具的 execute 是协程。两者要碰头，
就得有一个一直转着的事件循环。这一层只管这个：起一个循环，把协程丢进去，
等结果。
"""
import asyncio
import threading


class EventLoop:
    """后台线程上的事件循环。

    工具的 execute 都在这个循环里跑，阻塞的活儿工具自己用 asyncio.to_thread
    丢到线程里。一个工具在跑不会卡住整个网关。
    """

    def __init__(self):
        self._loop = None
        self._thread = None
        self._lock = threading.Lock()

    def start(self):
        """起事件循环。已经在跑就什么都不做。"""
        with self._lock:
            if self._loop is not None:
                return
            self._loop = asyncio.new_event_loop()
            self._thread = threading.Thread(target=self._loop.run_forever,
                                            daemon=True)
            self._thread.start()

    def stop(self):
        """停事件循环。"""
        with self._lock:
            loop = self._loop
            thread = self._thread
            self._loop = None
            self._thread = None
        if loop is None:
            return
        loop.call_soon_threadsafe(loop.stop)
        if thread is not None:
            thread.join(timeout=5)
        loop.close()

    def submit(self, coro, timeout):
        """把协程丢到事件循环里跑，等结果。

        超时抛 concurrent.futures.TimeoutError。
        """
        self.start()
        future = asyncio.run_coroutine_threadsafe(coro, self._loop)
        return future.result(timeout)
