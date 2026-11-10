/*
 * tool_device_internal.h — 设备框架工具模块的内部约定
 *
 * 每个设备类一个 .c 文件，自带一张工具描述符表，用 AGENT_TOOL_MODULE_EXPORT 登记到通用
 * 模块表（声明在 utils/tool_json.h）。新增一类设备只要加一个 .c 文件和一个 Kconfig 开关。
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

/*
 * @brief  清空工具节点上一次的返回结果
 * @param  node 工具节点
 */
void agent_dev_reset_result(AgentToolNode_t node);

/*
 * @brief  把结果码与结果文本写回工具节点
 * @param  node   工具节点
 * @param  result 结果码
 * @param  text   结果文本
 */
void agent_dev_finish(AgentToolNode_t node, rt_err_t result, const char *text);

/*
 * @brief  向结果缓冲区追加一段格式化文本，越界时截断
 * @param  out   结果缓冲区
 * @param  size  缓冲区大小
 * @param  used  已用字节数，追加后更新
 * @param  fmt   格式串
 */
void agent_dev_append(char *out, rt_size_t size, rt_size_t *used, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/*
 * @brief  解析十进制或 0x 十六进制数字字符串
 * @param  text  数字文本
 * @param  value 解析出的数值
 * @return 成功返回 1，非法返回 0
 */
int agent_dev_parse_num(const char *text, long *value);

/*
 * @brief  取设备类号对应的类名
 * @param  type 设备类号
 * @return 类名，越界或未知时返回 "unknown"
 */
const char *agent_dev_class_name(int type);

/*
 * @brief  按类名查设备类号
 * @param  name 类名
 * @return 设备类号，未知类名返回 -1
 */
int agent_dev_class_by_name(const char *name);

/*
 * @brief  取设备类名表的长度
 * @return 类名个数
 */
int agent_dev_class_count(void);

/*
 * @brief  把设备标志位翻译成可读文本
 * @param  flags 标志位组合
 * @param  out   输出缓冲区
 * @param  size  输出缓冲区大小
 */
void agent_dev_decode_flags(rt_uint16_t flags, char *out, rt_size_t size);

/*
 * @brief  遍历设备框架里注册的所有设备
 * @param  callback 每个设备调用一次的回调
 * @param  arg      传给回调的上下文
 * @return 遍历到的设备数量
 */
int agent_dev_foreach(void (*callback)(rt_device_t dev, void *arg), void *arg);

/*
 * @brief  按 name 参数查找设备，可要求设备已打开
 * @param  args      工具参数 JSON 对象
 * @param  node      工具节点，失败时用于回填结果
 * @param  name_out  输出设备名
 * @param  need_open 是否要求设备处于已打开状态
 * @param  out       结果缓冲区
 * @param  used      已用字节数
 * @return 设备句柄，失败返回 RT_NULL
 */
rt_device_t agent_dev_find(cJSON *args, AgentToolNode_t node, const char **name_out,
                           rt_bool_t need_open, char *out, rt_size_t *used);

/*
 * @brief  查找设备并校验它属于指定设备类
 * @param  args         工具参数 JSON 对象
 * @param  node         工具节点，失败时用于回填结果
 * @param  name_out     输出设备名
 * @param  class_id     期望的设备类号
 * @param  class_label  期望类名，用于错误文本
 * @param  need_open    是否要求设备处于已打开状态
 * @param  out          结果缓冲区
 * @param  used         已用字节数
 * @return 设备句柄，失败返回 RT_NULL
 */
rt_device_t agent_dev_find_class(cJSON *args, AgentToolNode_t node, const char **name_out,
                                 int class_id, const char *class_label, rt_bool_t need_open,
                                 char *out, rt_size_t *used);

/*
 * @brief  把十六进制文本解码成字节数组
 * @param  text     十六进制文本，允许空格、冒号、逗号分隔
 * @param  buffer   输出字节数组
 * @param  max      输出数组容量
 * @param  err      错误文本缓冲区
 * @param  err_size 错误文本缓冲区大小
 * @return 解码字节数，非法返回 -RT_EINVAL
 */
int agent_dev_hex_decode(const char *text, rt_uint8_t *buffer, rt_size_t max,
                         char *err, rt_size_t err_size);

/*
 * @brief  在结果里追加 hex 与可打印文本两行预览
 * @param  out   结果缓冲区
 * @param  size  结果缓冲区大小
 * @param  used  已用字节数
 * @param  data  数据字节
 * @param  len   数据长度
 */
void agent_dev_hex_preview(char *out, rt_size_t size, rt_size_t *used,
                           const rt_uint8_t *data, rt_size_t len);

/* 各模块的工具执行函数 */

/*
 * @brief  列出设备框架里注册的所有设备，可按设备类过滤
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_device_list(cJSON *args, AgentToolNode_t node);

/*
 * @brief  显示一个设备的类、标志、打开状态与引用计数
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_device_info(cJSON *args, AgentToolNode_t node);

/*
 * @brief  按名字打开一个设备，可指定读写模式与打开标志
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_device_open(cJSON *args, AgentToolNode_t node);

/*
 * @brief  关闭一个已打开的设备
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_device_close(cJSON *args, AgentToolNode_t node);

/*
 * @brief  从一个已打开的设备读取数据
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_device_read(cJSON *args, AgentToolNode_t node);

/*
 * @brief  向一个已打开的设备写入文本或十六进制数据
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_device_write(cJSON *args, AgentToolNode_t node);

/*
 * @brief  在一个已打开的设备上执行框架控制命令
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_device_control(cJSON *args, AgentToolNode_t node);

/*
 * @brief  设置一个 GPIO 引脚的方向与上下拉配置
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_pin_mode(cJSON *args, AgentToolNode_t node);

/*
 * @brief  把一个 GPIO 引脚驱动为高或低电平
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_pin_write(cJSON *args, AgentToolNode_t node);

/*
 * @brief  读取一个 GPIO 引脚当前的电平
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_pin_read(cJSON *args, AgentToolNode_t node);

/*
 * @brief  设置一个 PWM 通道的周期与高电平脉宽
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_pwm_set(cJSON *args, AgentToolNode_t node);

/*
 * @brief  读回一个 PWM 通道的周期与脉宽
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_pwm_get(cJSON *args, AgentToolNode_t node);

/*
 * @brief  启动一个 PWM 通道
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_pwm_enable(cJSON *args, AgentToolNode_t node);

/*
 * @brief  停止一个 PWM 通道
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_pwm_disable(cJSON *args, AgentToolNode_t node);

/*
 * @brief  采样一个 ADC 通道，返回原始码与电压
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_adc_read(cJSON *args, AgentToolNode_t node);

/*
 * @brief  向一个 DAC 通道输出原始码
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_dac_write(cJSON *args, AgentToolNode_t node);

/*
 * @brief  使能一个 DAC 通道
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_dac_enable(cJSON *args, AgentToolNode_t node);

/*
 * @brief  关闭一个 DAC 通道
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_dac_disable(cJSON *args, AgentToolNode_t node);

/*
 * @brief  向一个 I2C 从机写入一段字节
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_i2c_write(cJSON *args, AgentToolNode_t node);

/*
 * @brief  从一个 I2C 从机读取一段字节
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_i2c_read(cJSON *args, AgentToolNode_t node);

/*
 * @brief  在一次 I2C 传输里先写后读
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_i2c_writeread(cJSON *args, AgentToolNode_t node);

/*
 * @brief  配置一个 SPI 从机设备的工作参数
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_spi_configure(cJSON *args, AgentToolNode_t node);

/*
 * @brief  在一个 SPI 从机设备上做全双工收发
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_spi_transfer(cJSON *args, AgentToolNode_t node);

/*
 * @brief  读取一次传感器数据并解码成带单位的读数
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_sensor_read(cJSON *args, AgentToolNode_t node);

/*
 * @brief  显示一个传感器的型号、类、厂商、单位与量程
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_sensor_info(cJSON *args, AgentToolNode_t node);

/*
 * @brief  执行一条可带标量参数的传感器控制命令
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_sensor_control(cJSON *args, AgentToolNode_t node);

/*
 * @brief  设置一个 LED 的开关、切换或闪烁状态
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_led_set(cJSON *args, AgentToolNode_t node);

/*
 * @brief  读回一个 LED 当前的状态名
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_led_get(cJSON *args, AgentToolNode_t node);

/*
 * @brief  设置一个 LED 的亮度等级
 * @param  args 工具参数 JSON 对象
 * @param  node 工具节点，用于回填结果
 */
void tool_led_brightness(cJSON *args, AgentToolNode_t node);

#endif /* __AGENT_TOOL_DEVICE_INTERNAL_H__ */
