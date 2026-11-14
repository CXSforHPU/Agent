package com.agentconsole.card;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;

import java.util.List;

/**
 * 能力卡记录。
 *
 * <p>一个能力卡可以绑定多台设备（device.card_id 多行指向同一张卡），
 * 一台设备只能绑定一张卡（device.card_id 单列）。删除卡时绑定它的设备置空。
 *
 * @param id          主键
 * @param userId      创建者，只有创建者能改能删
 * @param name        卡名
 * @param description 描述
 * @param version     版本号
 * @param skills      技能列表
 * @param createdAt   创建时间
 * @param updatedAt   更新时间
 * @param deviceCount 当前绑定的设备数（查询时统计）
 */
@JsonIgnoreProperties(ignoreUnknown = true)
public record CapabilityCard(long id, long userId, String name, String description,
                             String version, List<Skill> skills, String createdAt,
                             String updatedAt, long deviceCount) {

    /** 卡上的一个技能。 */
    @JsonIgnoreProperties(ignoreUnknown = true)
    public record Skill(String id, String name, String description) {
    }

    /** 换绑定数。 */
    public CapabilityCard withDeviceCount(long count) {
        return new CapabilityCard(id, userId, name, description, version, skills,
                createdAt, updatedAt, count);
    }
}
