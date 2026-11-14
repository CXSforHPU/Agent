-- SQLite 方言。spring.sql.init 每次启动都会跑，所以全部 IF NOT EXISTS。
-- 老库补列由 SchemaMigrator 负责（ALTER TABLE ADD COLUMN），这里不依赖 drop。

CREATE TABLE IF NOT EXISTS app_user (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT    NOT NULL UNIQUE,
    password_hash TEXT    NOT NULL,
    salt          TEXT    NOT NULL,
    created_at    TEXT    NOT NULL DEFAULT (datetime('now', 'localtime'))
);

-- 能力卡：一个卡可绑定多台设备，一台设备只能绑一张（device.card_id 单列保证）。
-- 删除卡时设备置空（见 CapabilityCardService.delete，与 FK ON DELETE SET NULL 一致）。
CREATE TABLE IF NOT EXISTS capability_card (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER NOT NULL,
    name        TEXT    NOT NULL,
    description TEXT    NOT NULL DEFAULT '',
    version     TEXT    NOT NULL DEFAULT '1.0.0',
    -- JSON 数组：[{"id":"skill-1","name":"...","description":"..."}]
    skills      TEXT    NOT NULL DEFAULT '[]',
    created_at  TEXT    NOT NULL DEFAULT (datetime('now', 'localtime')),
    updated_at  TEXT    NOT NULL DEFAULT (datetime('now', 'localtime')),
    FOREIGN KEY (user_id) REFERENCES app_user (id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_capability_card_user ON capability_card (user_id);

CREATE TABLE IF NOT EXISTS device (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id       INTEGER NOT NULL,
    name          TEXT    NOT NULL,
    -- 端侧 agent 的逻辑地址 ws://设备ID，不是网络 URL。
    -- 板端用同一个 ID 主动连 Web 完成激活，能力卡由 Web 按绑定关系下发。
    address       TEXT    NOT NULL,
    group_name    TEXT    NOT NULL DEFAULT '',
    note          TEXT    NOT NULL DEFAULT '',
    enabled       INTEGER NOT NULL DEFAULT 1,
    created_at    TEXT    NOT NULL DEFAULT (datetime('now', 'localtime')),
    last_seen_at  TEXT,
    online        INTEGER NOT NULL DEFAULT 0,
    -- 激活生命周期：unactivated 未激活 / activating 激活中 / activated 已激活。
    -- 在线与否是实时连接状态，不落库，读取时按 WebSocket 会话计算。
    activation    TEXT    NOT NULL DEFAULT 'unactivated',
    -- 设备登录凭据：只存 PBKDF2 摘要 + 盐，绝不存明文密码。
    cred_username TEXT,
    cred_salt     TEXT,
    cred_hash     TEXT,
    -- 激活成功后一次性生成的设备 token，唯一且不可猜测；换 token 必须先取消激活。
    device_token  TEXT,
    activated_at  TEXT,
    -- 绑定的能力卡；一台设备只有一张，删卡时置空。
    card_id       INTEGER,
    FOREIGN KEY (user_id)  REFERENCES app_user (id)      ON DELETE CASCADE,
    FOREIGN KEY (card_id)  REFERENCES capability_card (id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_device_user ON device (user_id);

-- 会话：device_id 为 NULL 表示网站侧 agent 的会话
CREATE TABLE IF NOT EXISTS conversation (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id       INTEGER NOT NULL,
    device_id     INTEGER,
    agent_kind    TEXT    NOT NULL DEFAULT 'website',
    title         TEXT    NOT NULL DEFAULT '新会话',
    created_at    TEXT    NOT NULL DEFAULT (datetime('now', 'localtime')),
    updated_at    TEXT    NOT NULL DEFAULT (datetime('now', 'localtime')),
    FOREIGN KEY (user_id)    REFERENCES app_user (id) ON DELETE CASCADE,
    FOREIGN KEY (device_id)  REFERENCES device (id)   ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_conversation_user ON conversation (user_id, updated_at DESC);

CREATE TABLE IF NOT EXISTS message (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    conversation_id INTEGER NOT NULL,
    role            TEXT    NOT NULL,
    content         TEXT    NOT NULL,
    created_at      TEXT    NOT NULL DEFAULT (datetime('now', 'localtime')),
    FOREIGN KEY (conversation_id) REFERENCES conversation (id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_message_conversation ON message (conversation_id, id);

-- 用户级设置。key 是已知白名单，value 存原文。
-- 整个 SQLite 文件在 .gitignore 的 /resource/web/server/data/ 下，个人配置不进版本库。
CREATE TABLE IF NOT EXISTS user_setting (
    user_id  INTEGER NOT NULL,
    key      TEXT    NOT NULL,
    value    TEXT    NOT NULL,
    updated_at TEXT   NOT NULL DEFAULT (datetime('now', 'localtime')),
    PRIMARY KEY (user_id, key),
    FOREIGN KEY (user_id) REFERENCES app_user (id) ON DELETE CASCADE
);

-- 运维面板的宿主级设置，目前只有文件服务器的端口。key 是已知白名单，value 存原文。
-- 跟 user_setting 一样在 .gitignore 的 /resource/web/server/data/ 下，个人配置不进版本库。
CREATE TABLE IF NOT EXISTS ops_setting (
    key        TEXT NOT NULL PRIMARY KEY,
    value      TEXT NOT NULL,
    updated_at TEXT NOT NULL DEFAULT (datetime('now', 'localtime'))
);

-- 端侧 agent 自发上报的事件：MQTT 告警、传感器越限等等。
-- 板端沿 A2A WebSocket 主动发过来，落库后推给正在听 SSE 的网页。
CREATE TABLE IF NOT EXISTS device_event (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id  INTEGER NOT NULL,
    kind       TEXT    NOT NULL DEFAULT 'report',
    text       TEXT    NOT NULL,
    created_at TEXT    NOT NULL DEFAULT (datetime('now', 'localtime')),
    FOREIGN KEY (device_id) REFERENCES device (id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_device_event_device ON device_event (device_id, id DESC);
