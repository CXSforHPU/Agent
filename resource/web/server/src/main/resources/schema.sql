-- SQLite 方言。spring.sql.init 每次启动都会跑，所以全部 IF NOT EXISTS。

CREATE TABLE IF NOT EXISTS app_user (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT    NOT NULL UNIQUE,
    password_hash TEXT    NOT NULL,
    salt          TEXT    NOT NULL,
    created_at    TEXT    NOT NULL DEFAULT (datetime('now', 'localtime'))
);

CREATE TABLE IF NOT EXISTS device (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id       INTEGER NOT NULL,
    name          TEXT    NOT NULL,
    -- 端侧 agent 的服务根地址，能力卡在 <address>/.well-known/agent.json
    address       TEXT    NOT NULL,
    group_name    TEXT    NOT NULL DEFAULT '',
    note          TEXT    NOT NULL DEFAULT '',
    enabled       INTEGER NOT NULL DEFAULT 1,
    created_at    TEXT    NOT NULL DEFAULT (datetime('now', 'localtime')),
    last_seen_at  TEXT,
    online        INTEGER NOT NULL DEFAULT 0,
    FOREIGN KEY (user_id) REFERENCES app_user (id) ON DELETE CASCADE
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
