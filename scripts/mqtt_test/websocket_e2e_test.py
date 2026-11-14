#!/usr/bin/env python3
"""QEMU ↔ 当前 Web 的 A2A WebSocket 激活联调。

不自带后端：直接打已经在跑的控制台（默认 http://127.0.0.1:8080），
只删本脚本自己创建的设备与能力卡，不动账号原有数据。流程：

  1. 注册/登录测试账号，建设备 ws://qemu-agent 并填激活账号密码，绑一张能力卡
  2. 主机侧 WebSocket 客户端先用错密码试一次，必须被拒（bad_credential）
  3. QEMU 用 SD 副本 + user 网络启动，串口 5556；写随机种子与激活凭据文件
  4. 串口执行 a2a_activate → 服务端签发一次性设备 token → 板端落盘并长连
  5. API 断言：设备 status=online、探活 online 且拿到真实能力卡、任务有回包
  6. 主机侧再用正确密码连一次，必须被拒（already_online）
  7. 另建一台设备走两次主机侧激活：第一次成功，第二次必须 already_activated

判据见 docs/testing.md 第 8 节。LLM 失败只说明最终结果回传通了，
不能算计算任务成功，会单列打印。结果落在 ignored 的 target/ws-test/。
用法：
  python websocket_e2e_test.py                 # 结束清理自建设备并停 QEMU
  CONSOLE_USER=webtest CONSOLE_PASSWORD=... \\
      python websocket_e2e_test.py --keep       # 落到指定账号，保留 QEMU 供浏览器看
"""
import json
import os
import secrets
import shutil
import socket
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

PACKAGE = Path(__file__).resolve().parents[2]
ROOT = PACKAGE.parents[1]
SERVER = PACKAGE / 'resource/web/server'
ENV = Path('C:/Users/Public/Downloads/rtt/env-windows')
WORK = SERVER / 'target/ws-test'
VENV_SITE = PACKAGE / 'resource/file_server/.venv/Lib/site-packages'

BASE = os.environ.get('CONSOLE_URL', 'http://127.0.0.1:8080').rstrip('/')
BOARD_HOST = os.environ.get('QEMU_BOARD_HOST', '192.168.137.1')
BOARD_PORT = int(os.environ.get('QEMU_BOARD_PORT', '8080'))
SERIAL_PORT = int(os.environ.get('QEMU_SERIAL_PORT', '5556'))
DEVICE_ID = 'qemu-agent'
DEVICE_ADDR = 'ws://' + DEVICE_ID
CARD_NAME = 'QEMU 激活联调能力卡'

# 串口里出现这些串就算过关；密码/token 只进脱敏日志
MARK_ACTIVATED = 'a2a_activate: [OK]'
MARK_CONNECTED = 'A2A WebSocket connected.'
MARK_NO_CRED = '[NO-CRED]'


def api(path, body=None, token=None, method='POST'):
    headers = {'Content-Type': 'application/json'}
    if token:
        headers['Authorization'] = 'Bearer ' + token
    request = urllib.request.Request(
        BASE + '/api' + path,
        data=json.dumps(body).encode() if body is not None else None,
        headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        raw = error.read()
        try:
            return error.code, json.loads(raw)
        except ValueError:
            return error.code, {'success': False,
                                'message': raw[:200].decode('utf-8', 'replace')}


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print('PASS ' + message, flush=True)


def redact(text, *secrets_to_hide):
    for value in secrets_to_hide:
        if value:
            text = text.replace(value, '[REDACTED]')
    return text


class Serial:
    """串口终端：等提示符、发命令、按标记等待，输出一律脱敏。"""

    def __init__(self, port, logfile, *secrets_to_hide):
        self.sock = socket.create_connection(('127.0.0.1', port), 10)
        self.sock.settimeout(1)
        self.log = open(logfile, 'ab')
        self.secrets = secrets_to_hide
        self.buffer = b''

    def _pump(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                raise EOFError('串口被关闭')
            self.buffer += chunk
            self.log.write(redact(chunk.decode('utf-8', 'replace'),
                                  *self.secrets).encode())
            self.log.flush()

    def wait(self, marker, seconds=60):
        want = marker.encode()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if want in self.buffer:
                return
            self._pump(1)
        tail = redact(self.buffer[-300:].decode('utf-8', 'replace'), *self.secrets)
        raise TimeoutError('串口等待超时: %r，最近输出: %s' % (marker, tail))

    def send(self, line):
        self.buffer = b''
        self.log.write(('<<< SEND: ' + redact(line, *self.secrets) + ' >>>\n').encode())
        self.log.flush()
        self.sock.sendall(line.encode() + b'\r\n')

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
        self.log.close()


def ws_hello(device_id, username=None, password=None, token=None):
    """主机侧 WebSocket 客户端，发一帧 hello 返回服务端应答。"""
    sys.path.insert(0, str(VENV_SITE))
    from websockets.sync.client import connect
    hello = {'type': 'hello', 'deviceId': device_id}
    if username:
        hello['username'] = username
        hello['password'] = password
    else:
        hello['token'] = token
    with connect('ws://127.0.0.1:8080/api/a2a/ws', open_timeout=5,
                 close_timeout=5) as websocket:
        websocket.send(json.dumps(hello))
        return json.loads(websocket.recv(timeout=5))


def register_or_login():
    user = os.environ.get('CONSOLE_USER')
    password = os.environ.get('CONSOLE_PASSWORD')
    if user:
        status, payload = api('/auth/login', {'username': user, 'password': password})
        expect(payload.get('success'), '登录已有测试账号 ' + user)
        return user, payload['data']['token'], False
    user = 'qemu_e2e_' + secrets.token_hex(4)
    password = secrets.token_hex(12)
    status, payload = api('/auth/register', {'username': user, 'password': password})
    expect(payload.get('success'), '注册独立测试账号 ' + user)
    return user, payload['data']['token'], True


def delete_created(token, device_ids, card_ids):
    """只删本脚本创建的设备与能力卡，账号原有数据不碰。"""
    for device_id in device_ids:
        api('/devices/' + str(device_id), token=token, method='DELETE')
    for card_id in card_ids:
        api('/cards/' + str(card_id), token=token, method='DELETE')


def delete_temp_account(username):
    """临时账号连同它的会话、设置一并删掉，避免库里攒测试行。"""
    database = SERVER / 'data/agent-console.db'
    if not database.exists():
        return
    connection = sqlite3.connect(database)
    try:
        cursor = connection.cursor()
        row = cursor.execute('SELECT id FROM app_user WHERE username = ?',
                             (username,)).fetchone()
        if row is None:
            return
        user_id = row[0]
        cursor.execute(
            'DELETE FROM message WHERE conversation_id IN '
            '(SELECT id FROM conversation WHERE user_id = ?)', (user_id,))
        cursor.execute('DELETE FROM conversation WHERE user_id = ?', (user_id,))
        cursor.execute('DELETE FROM user_setting WHERE user_id = ?', (user_id,))
        cursor.execute('DELETE FROM device_event WHERE device_id IN '
                       '(SELECT id FROM device WHERE user_id = ?)', (user_id,))
        cursor.execute('DELETE FROM device_push WHERE device_id IN '
                       '(SELECT id FROM device WHERE user_id = ?)', (user_id,))
        cursor.execute('DELETE FROM device WHERE user_id = ?', (user_id,))
        cursor.execute('DELETE FROM capability_card WHERE user_id = ?', (user_id,))
        cursor.execute('DELETE FROM app_user WHERE id = ?', (user_id,))
        connection.commit()
    finally:
        connection.close()


def main():
    keep = '--keep' in sys.argv[1:]
    WORK.mkdir(parents=True, exist_ok=True)
    password_board = 'QemuE2e' + secrets.token_hex(6)
    username_board = 'qemu-e2e-' + secrets.token_hex(3)
    password_once = 'OnceE2e' + secrets.token_hex(6)
    username_once = 'qemu-once-' + secrets.token_hex(3)
    seed = secrets.token_hex(24)          # 48 字节，板端随机种子要求正好 48
    serial = None
    qemu = None
    account = None
    account_token = None
    temp_account = False
    created_devices = []
    created_cards = []

    try:
        account, account_token, temp_account = register_or_login()
        if keep and temp_account:
            print('临时账号（--keep 保留，用它登录查看）: %s / %s'
                  % (account, os.environ.get('CONSOLE_PASSWORD') or '见注册输出'),
                  flush=True)

        # 同地址的旧测试设备与本脚本留下的孤儿能力卡先清掉，保证每次全新激活
        _, payload = api('/devices', token=account_token, method='GET')
        for device in payload.get('data') or []:
            if device['address'] == DEVICE_ADDR and device['name'].startswith('QEMU 激活联调'):
                api('/devices/' + str(device['id']), token=account_token, method='DELETE')
        _, payload = api('/cards', token=account_token, method='GET')
        for old_card in payload.get('data') or []:
            if old_card['name'] == CARD_NAME and not old_card.get('deviceCount'):
                api('/cards/' + str(old_card['id']), token=account_token, method='DELETE')

        status, payload = api('/devices', {
            'name': 'QEMU 激活联调', 'address': DEVICE_ADDR,
            'username': username_board, 'password': password_board}, account_token)
        expect(payload.get('success'), '新建设备并下发激活凭据')
        board_device = payload['data']['device']
        created_devices.append(board_device['id'])
        expect(board_device['status'] == 'activating',
               '新设备初始状态为激活中')
        activation = (payload.get('data') or {}).get('activation') or {}
        expect(activation.get('password') == password_board,
               '一次性激活配置只在创建响应里出现一次')
        expect(board_device.get('cardName') is None, '新设备未绑定能力卡')

        status, payload = api('/cards', {
            'name': CARD_NAME, 'description': 'e2e',
            'version': '1.0.0',
            'skills': [{'id': 's1', 'name': 'ping', 'description': ''}]},
            account_token)
        expect(payload.get('success'), '创建能力卡')
        card = payload['data']
        created_cards.append(card['id'])
        status, payload = api('/cards/' + str(card['id']) + '/bind',
                              {'deviceId': board_device['id']}, account_token)
        expect(payload.get('success') and payload['data']['deviceCount'] == 1,
               '一张卡绑定到设备')

        # 激活前：错密码必须被拒
        reply = ws_hello(DEVICE_ID, username=username_board, password='wrong-password')
        expect(reply.get('type') == 'error' and reply.get('code') == 'bad_credential',
               '激活前错密码被拒 bad_credential')

        # QEMU：SD 副本 + user 网络，不占 TAP 与 5555 终端
        shutil.copyfile(ROOT / 'sd.bin', WORK / 'sd.bin')
        qemu_log = open(WORK / 'qemu.log', 'wb')
        qemu = subprocess.Popen([
            str(ENV / 'tools/qemu/qemu64/qemu-system-arm.exe'),
            '-M', 'vexpress-a9', '-smp', 'cpus=2',
            '-kernel', str(ROOT / 'rtthread.bin'),
            '-serial', 'tcp:127.0.0.1:%d,server,nowait' % SERIAL_PORT,
            '-drive', 'file=' + str(WORK / 'sd.bin') + ',format=raw,if=sd',
            '-audiodev', 'none,id=audio0',
            '-net', 'nic,model=lan9118,netdev=net0',
            '-netdev', 'user,id=net0,net=192.168.137.0/24,host=192.168.137.1,'
                       'dhcpstart=192.168.137.100',
            '-display', 'none'], cwd=ROOT, stdout=qemu_log, stderr=qemu_log)
        (WORK / 'pids.json').write_text(json.dumps({'qemu': qemu.pid}))
        print('QEMU pid=%d serial=127.0.0.1:%d' % (qemu.pid, SERIAL_PORT), flush=True)

        serial = Serial(SERIAL_PORT, WORK / 'serial.log', password_board,
                        password_once, seed, account_token or '')
        serial.wait('msh />', 90)
        expect(True, '板端启动到 msh 提示符')

        # 凭据与随机种子写进 SD 卡；激活优先走凭据文件，不在命令行里带密码
        for name in ('agent_a2a_token', 'agent_a2a_endpoint', 'agent_a2a_cred',
                     'agent_a2a_seed'):
            serial.send('rm /' + name)
            serial._pump(0.5)
        serial.send('echo "%s" /agent_a2a_seed' % seed)
        serial._pump(0.5)
        serial.send('echo "%s %s" /agent_a2a_cred' % (username_board, password_board))
        serial._pump(0.5)

        serial.send('a2a_activate %s %d' % (BOARD_HOST, BOARD_PORT))
        try:
            serial.wait(MARK_ACTIVATED, 30)
        except TimeoutError:
            if MARK_NO_CRED.encode() not in serial.buffer:
                raise
            # 凭据文件没被 msh echo 正确写入时回退到命令行参数（测试密码，仅本地）
            serial.send('a2a_activate %s %d %s %s'
                        % (BOARD_HOST, BOARD_PORT, username_board, password_board))
            serial.wait(MARK_ACTIVATED, 60)
        expect(True, '板端 a2a_activate 激活成功并保存 token')

        # 激活后：设备应为已激活，尚未长连时是未在线
        _, payload = api('/devices', token=account_token, method='GET')
        row = next(d for d in payload['data'] if d['address'] == DEVICE_ADDR)
        expect(row['status'] in ('offline', 'online'),
               '激活后状态离开激活中（当前 ' + row['status'] + '）')

        try:
            serial.wait(MARK_CONNECTED, 25)
        except TimeoutError:
            serial.send('main_loop_entry')
            serial.wait(MARK_CONNECTED, 60)
        expect(True, '板端凭 token 建立长连接 A2A WebSocket connected')

        # Web 侧在线状态
        deadline = time.monotonic() + 30
        online = False
        while time.monotonic() < deadline:
            _, payload = api('/devices', token=account_token, method='GET')
            row = next(d for d in payload['data'] if d['address'] == DEVICE_ADDR)
            online = row['status'] == 'online'
            if online:
                break
            time.sleep(1)
        expect(online, '当前 Web 设备状态显示在线')

        _, payload = api('/devices/ws/status', token=account_token, method='GET')
        conn = next(d for d in payload['data']['devices'] if d['deviceId'] == DEVICE_ID)
        expect(conn['connected'], 'WebSocket 连接窗口显示已连接')

        status, payload = api('/devices/%d/probe' % row['id'], {}, account_token)
        expect(payload.get('success') and payload['data']['online'],
               '探活 online=true')
        card_payload = payload['data'].get('card') or {}
        # 板端返回的应是服务端在 welcome 后推下去的那张绑定卡，不是 Kconfig 默认名
        expect(card_payload.get('name') == CARD_NAME,
               '探活返回的是绑定的能力卡: ' + str(card_payload.get('name')))
        expect((card_payload.get('capabilities') or {}).get('pushNotifications') is True,
               '能力卡声明 pushNotifications=true')
        expect(len(card_payload.get('skills') or []) == 1,
               '绑定卡的技能随卡下发到板端')

        # 经 Web 发任务，等 final 回包
        status, payload = api('/chat', {
            'deviceId': row['id'],
            'message': 'Reply with only the result of 7 + 5.'}, account_token)
        reply = (payload.get('data') or {}).get('reply', '')
        (WORK / 'task-result.json').write_text(
            json.dumps({'success': payload.get('success'), 'reply': reply},
                       ensure_ascii=False, indent=2), encoding='utf-8')
        expect(payload.get('success') and reply,
               'Web → WebSocket → 板端 → Web 任务回传非空 final')
        print('TASK RESULT: ' + reply, flush=True)
        if 'failed' in reply.lower() or 'error' in reply.lower():
            print('WARN 任务回包是失败文案，只证明链路通，LLM 未成功', flush=True)

        # 在线时用正确密码再连一次，必须被拒
        reply = ws_hello(DEVICE_ID, username=username_board, password=password_board)
        expect(reply.get('type') == 'error'
               and reply.get('code') in ('already_online', 'already_activated'),
               '已连接设备重复激活被拒 ' + str(reply.get('code')))

        # 主机侧走两遍激活：第二次必须 already_activated（不碰板端这条连接）
        once_id = 'qemu-once-' + secrets.token_hex(3)
        status, payload = api('/devices', {
            'name': 'QEMU 一次性激活自检', 'address': 'ws://' + once_id,
            'username': username_once, 'password': password_once}, account_token)
        expect(payload.get('success'), '建第二台设备做一次性激活自检')
        created_devices.append(payload['data']['device']['id'])
        first = ws_hello(once_id, username=username_once, password=password_once)
        expect(first.get('type') == 'welcome' and len(first.get('token', '')) >= 16,
               '第一次激活签发设备 token')
        second = ws_hello(once_id, username=username_once, password=password_once)
        expect(second.get('type') == 'error'
               and second.get('code') == 'already_activated',
               '同一设备第二次激活被拒 already_activated')

        print('\nALL PASS', flush=True)
        if keep:
            print('--keep：QEMU 保留 pid=%d，设备 %s 留在账号 %s'
                  % (qemu.pid, DEVICE_ADDR, account), flush=True)
    finally:
        if serial is not None:
            serial.close()
        if not keep and qemu is not None and qemu.poll() is None:
            qemu.terminate()
            try:
                qemu.wait(timeout=10)
            except subprocess.TimeoutExpired:
                qemu.kill()
            print('已停止本次 QEMU', flush=True)
        if account_token is not None and not keep:
            try:
                delete_created(account_token, created_devices, created_cards)
                print('已清理本次创建的设备与能力卡', flush=True)
                if temp_account:
                    delete_temp_account(account)
                    print('已删除临时账号 ' + account, flush=True)
            except Exception as error:  # 清理失败不能盖掉真正的测试结果
                print('WARN 清理测试数据失败: ' + str(error), flush=True)
            pids = WORK / 'pids.json'
            if pids.exists():
                pids.unlink()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print('FAIL ' + str(error), flush=True)
        sys.exit(1)
