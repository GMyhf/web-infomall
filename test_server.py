"""Quick smoke test for the replay server."""
import sys, os, time, threading, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import server
from http.server import HTTPServer

store = server.ArchiveStore('archive.db')
server.ReplayHandler.store = store

srv = HTTPServer(('127.0.0.1', 8099), server.ReplayHandler)

def run_test(path, description):
    try:
        resp = urllib.request.urlopen(f'http://127.0.0.1:8099{path}')
        html = resp.read().decode('utf-8')
        status = resp.status
        size = len(html)
        # Extract title
        title = 'N/A'
        if '<title>' in html:
            title = html[html.find('<title>')+7:html.find('</title>')]
        print(f'  [{status}] {description}')
        print(f'    Title: {title}')
        print(f'    Size: {size} bytes')
        return True
    except Exception as e:
        print(f'  [ERR] {description}: {e}')
        return False

# Start server thread
t = threading.Thread(target=srv.serve_forever, daemon=True)
t.start()
time.sleep(0.3)

print('=== Server Smoke Tests ===')
run_test('/', 'Homepage')
run_test('/search?q=http://sina', 'Search')
run_test('/replay?url=http://dailynews.sina.com.cn/c/2005-03-30/05245500453s.shtml', 'Replay')
run_test('/calendar?url=http://dailynews.sina.com.cn/c/2005-03-30/05245500453s.shtml', 'Calendar')
run_test('/replay?url=http://nonexistent.example.com/foo', '404')
run_test('/stats', 'Stats API')

srv.shutdown()
print('Done!')
store.close()
