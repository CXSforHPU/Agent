/*
 * tool_device_internal.h — 设备框架工具模块的内部约定
 *
 * 每个设备类一个 .c 文件，自带一张工具描述符表，用 AGENT_TOOL_MODULE_EXPORT
 * 把自己登记到通用模块表（声明在 utils/tool_json.h，实现在 src/utils/tool_json.c）。
 * 新增一类设备只要加一个 .c 文件和一个 Kconfig 开关，不用改中心文件。
 */

#ifndef __AGENT_TOOL_DEVICE_INTERNAL_H__
#define __AGENT_TOOL_DEVICE_INTERNAL_H__

#include <rtthread.h>
#include "cJSON.h"
#include "tool_base.h"
#include "utils/tool_json.h"

#define DEVICE_OUT_SIZE     1024    /* 单条工具结果的文本缓冲 */
#define DEVICE_READ_MAX     512     /* 字符设备单次读取上限，单位字节 */
#define DEVICE_IO_MAX       2048    /* 单次传输上限，单位字节 */
#define DEVICE_PREVIEW_MAX  128     /* 读结果里 hex/text 预览的字节数 */

/* tool_device_common.c — 各模块共用的助手 */
void agent_dev_reset_result(AgentToolNode_t node);
void agent_dev_finish(AgentToolNode_t node, rt_err_t result, const char *text);
void agent_dev_append(char *out, rt_size_t size, rt_size_t *used, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
    ;
int agent_dev_parse_num(const char *text, long *value);
const char *agent_dev_class_name(int type);
int agent_dev_class_by_name(const char *name);
int agent_dev_class_count(void);
void agent_dev_decode_flags(rt_uint16_t flags, char *out, rt_size_t size);
int agent_dev_foreach(void (*callback)(rt_device_t dev, void *arg), void *arg);

/* 取 name 参数并查找设备。失败时已经写好结果，返回 RT_NULL。 */
rt_device_t agent_dev_find(cJSON *args, AgentToolNode_t node, const char **name_out,
                           rt_bool_t need_open, char *out, rt_size_t *used);

/* 在 agent_dev_find 之上再检查设备类，类不对时给出期望类名的错误文本。 */
rt_device_t agent_dev_find_class(cJSON *args, AgentToolNode_t node, const char **name_out,
                                 int class_id, const char *class_label, rt_bool_t need_open,
                                 char *out, rt_size_t *used);

/* 十六进制文本解码。返回解码字节数，负值是 -RT_EINVAL 类的错误，错误文本写进 err。 */
int agent_dev_hex_decode(const char *text, rt_uint8_t *buffer, rt_size_t max,
                         char *err, rt_size_t err_size);

/* 读结果的两行预览：hex 与可打印文本。 */
void agent_dev_hex_preview(char *out, rt_size_t size, rt_size_t *used,
                           const rt_uint8_t *data, rt_size_t len);

/* 各模块的工具执行函数 */
void tool_device_list(cJSON *args, AgentToolNode_t node);
void tool_device_info(cJSON *args, AgentToolNode_t node);
void tool_device_open(cJSON *args, AgentToolNode_t node);
void tool_device_close(cJSON *args, AgentToolNode_t node);
void tool_device_read(cJSON *args, AgentToolNode_t node);
void tool_device_write(cJSON *args, AgentToolNode_t node);
void tool_device_control(cJSON *args, AgentToolNode_t node);

void tool_pin_mode(cJSON *args, AgentToolNode_t node);
void tool_pin_write(cJSON *args, AgentToolNode_t node);
void tool_pin_read(cJSON *args, AgentToolNode_t node);

void tool_pwm_set(cJSON *args, AgentToolNode_t node);
void tool_pwm_get(cJSON *args, AgentToolNode_t node);
void tool_pwm_enable(cJSON *args, AgentToolNode_t node);
void tool_pwm_disable(cJSON *args, AgentToolNode_t node);

void tool_adc_read(cJSON *args, AgentToolNode_t node);

void tool_dac_write(cJSON *args, AgentToolNode_t node);
void tool_dac_enable(cJSON *args, AgentToolNode_t node);
void tool_dac_disable(cJSON *args, AgentToolNode_t node);

void tool_i2c_write(cJSON *args, AgentToolNode_t node);
void tool_i2c_read(cJSON *args, AgentToolNode_t node);
void tool_i2c_writeread(cJSON *args, AgentToolNode_t node);

void tool_spi_configure(cJSON *args, AgentToolNode_t node);
void tool_spi_transfer(cJSON *args, AgentToolNode_t node);

void tool_sensor_read(cJSON *args, AgentToolNode_t node);
void tool_sensor_info(cJSON *args, AgentToolNode_t node);
void tool_sensor_control(cJSON *args, AgentToolNode_t node);

void tool_led_set(cJSON *args, AgentToolNode_t node);
void tool_led_get(cJSON *args, AgentToolNode_t node);
void tool_led_brightness(cJSON *args, AgentToolNode_t node);

#endif /* __AGENT_TOOL_DEVICE_INTERNAL_H__ */
