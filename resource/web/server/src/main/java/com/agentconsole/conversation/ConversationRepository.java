package com.agentconsole.conversation;

import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.jdbc.support.GeneratedKeyHolder;
import org.springframework.jdbc.support.KeyHolder;
import org.springframework.stereotype.Repository;

import java.sql.PreparedStatement;
import java.sql.Statement;
import java.util.List;
import java.util.Optional;

@Repository
public class ConversationRepository {

    private final JdbcTemplate jdbc;

    public ConversationRepository(JdbcTemplate jdbc) {
        this.jdbc = jdbc;
    }

    private Conversation mapConversation(java.sql.ResultSet rs, int i) throws java.sql.SQLException {
        long deviceId = rs.getLong("device_id");
        // 必须紧跟 getLong 取 wasNull，参数列表里其它 getLong 会把它冲掉
        boolean deviceNull = rs.wasNull();
        return new Conversation(rs.getLong("id"), rs.getLong("user_id"),
                deviceNull ? null : deviceId, rs.getString("agent_kind"),
                rs.getString("title"), rs.getString("created_at"), rs.getString("updated_at"));
    }

    private Message mapMessage(java.sql.ResultSet rs, int i) throws java.sql.SQLException {
        return new Message(rs.getLong("id"), rs.getLong("conversation_id"),
                rs.getString("role"), rs.getString("content"), rs.getString("created_at"));
    }

    public List<Conversation> findByUser(long userId) {
        return jdbc.query("SELECT id, user_id, device_id, agent_kind, title, created_at, updated_at "
                        + "FROM conversation WHERE user_id = ? ORDER BY updated_at DESC, id DESC",
                this::mapConversation, userId);
    }

    public Optional<Conversation> findById(long id, long userId) {
        var rows = jdbc.query("SELECT id, user_id, device_id, agent_kind, title, created_at, updated_at "
                        + "FROM conversation WHERE id = ? AND user_id = ?",
                this::mapConversation, id, userId);
        return rows.stream().findFirst();
    }

    public Conversation create(long userId, Long deviceId, String agentKind, String title) {
        KeyHolder keys = new GeneratedKeyHolder();
        jdbc.update(con -> {
            PreparedStatement ps = con.prepareStatement(
                    "INSERT INTO conversation (user_id, device_id, agent_kind, title) VALUES (?, ?, ?, ?)",
                    Statement.RETURN_GENERATED_KEYS);
            ps.setLong(1, userId);
            if (deviceId == null) {
                ps.setObject(2, null);
            } else {
                ps.setLong(2, deviceId);
            }
            ps.setString(3, agentKind);
            ps.setString(4, title);
            return ps;
        }, keys);
        return findById(keys.getKey().longValue(), userId).orElseThrow();
    }

    public void touch(long id) {
        jdbc.update("UPDATE conversation SET updated_at = datetime('now','localtime') WHERE id = ?", id);
    }

    public void rename(long id, String title) {
        jdbc.update("UPDATE conversation SET title = ? WHERE id = ?", title, id);
    }

    public boolean delete(long id, long userId) {
        return jdbc.update("DELETE FROM conversation WHERE id = ? AND user_id = ?", id, userId) > 0;
    }

    public List<Message> findMessages(long conversationId) {
        return jdbc.query("SELECT id, conversation_id, role, content, created_at FROM message "
                        + "WHERE conversation_id = ? ORDER BY id",
                this::mapMessage, conversationId);
    }

    public Message appendMessage(long conversationId, String role, String content) {
        jdbc.update("INSERT INTO message (conversation_id, role, content) VALUES (?, ?, ?)",
                conversationId, role, content);
        touch(conversationId);
        var rows = jdbc.query("SELECT id, conversation_id, role, content, created_at FROM message "
                        + "WHERE conversation_id = ? ORDER BY id DESC LIMIT 1",
                this::mapMessage, conversationId);
        return rows.get(0);
    }
}
