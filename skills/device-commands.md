# Device control command reference

Use with the `device` skill. Pass these names as `cmd` to `device_control`; the tool computes the command number. Marks: **value** = returns data; **no arg** = call without `arg`; **unsupported** = the tool refuses it because its argument is a driver-specific structure.

## Command numbering

Framework commands are small constants. Class commands are `(class_id + 1) * 0x100 + n`. Class ids: char 0, block 1, netif 2, mtd 3, can 4, rtc 5, sound 6, graphic 7, i2cbus 8, usbdevice 9, usbhost 10, usbotg 11, spibus 12, spidevice 13, sdio 14, pm 15, pipe 16, portal 17, timer 18, misc 19, sensor 20, touch 21, phy 22, security 23, wlan 24, pin 25, adc 26, dac 27, wdt 28, pwm 29, bus 30. Example: block commands start at 0x200, so `BLK_GETGEOME` is 0x201.

## Base commands (any class)

| Name | Number | Mark | Note |
|---|---|---|---|
| RESUME | 0x01 | unsupported | driver-specific argument |
| SUSPEND | 0x02 | unsupported | driver-specific argument |
| CONFIG | 0x03 | unsupported | driver-specific structure |
| CLOSE | 0x04 | unsupported | driver-specific structure |
| NOTIFY_SET | 0x05 | unsupported | callback structure |
| SET_INT | 0x06 | unsupported | driver-specific interrupt argument |
| CLR_INT | 0x07 | unsupported | driver-specific interrupt argument |
| GET_INT | 0x08 | unsupported | writes through a driver pointer |
| CONSOLE_OFLAG | 0x09 | unsupported | writes through a driver pointer |

## char (base 0x100)

| Name | Number | Mark | Note |
|---|---|---|---|
| CHAR_STREAM | 0x101 | unsupported | argument layout is driver-specific |

## block (base 0x200)

| Name | Number | Mark | Note |
|---|---|---|---|
| BLK_GETGEOME | 0x201 | value | returns `sector_count`, `bytes_per_sector`, `block_size` |
| BLK_SYNC | 0x202 | no arg | flushes cached writes to the medium |
| BLK_ERASE | 0x203 | unsupported | sector-range structure |
| BLK_AUTOREFRESH | 0x204 | unsupported | mode argument |
| BLK_PARTITION | 0x205 | unsupported | output structure |

## netif (base 0x300) and mtd (base 0x400)

| Name | Number | Mark | Note |
|---|---|---|---|
| NETIF_GETMAC | 0x301 | unsupported | writes through a MAC buffer pointer |
| MTD_FORMAT | 0x401 | unsupported | format argument structure; destructive |

## rtc (base 0x600)

| Name | Number | Mark | Note |
|---|---|---|---|
| RTC_GET_TIME | 0x601 | value | seconds since epoch |
| RTC_SET_TIME | 0x602 | needs `arg` | `arg` is the new time in seconds; changing the clock needs user authorization |
| RTC_GET_TIMEVAL | 0x603 | unsupported | struct timeval pointer |
| RTC_SET_TIMEVAL | 0x604 | unsupported | struct timeval pointer |
| RTC_GET_ALARM | 0x605 | unsupported | alarm structure pointer |
| RTC_SET_ALARM | 0x606 | unsupported | alarm structure pointer |
| RTC_GET_TIMESPEC | 0x607 | unsupported | timespec pointer |
| RTC_SET_TIMESPEC | 0x608 | unsupported | timespec pointer |
| RTC_GET_TIMERES | 0x609 | unsupported | resolution pointer |

## wdt (base 0x1D00)

| Name | Number | Mark | Note |
|---|---|---|---|
| WDT_GET_TIMEOUT | 0x1D01 | value | watchdog timeout in seconds |
| WDT_SET_TIMEOUT | 0x1D02 | needs `arg` | new timeout in seconds |
| WDT_GET_TIMELEFT | 0x1D03 | value | seconds left before reset |
| WDT_KEEPALIVE | 0x1D04 | no arg | feed the watchdog |
| WDT_START | 0x1D05 | no arg | enable watchdog resets |
| WDT_STOP | 0x1D06 | no arg | disable watchdog resets |

## graphic (absolute numbers)

| Name | Number | Mark | Note |
|---|---|---|---|
| CURSOR_SET_POSITION | 0x10 | unsupported | cursor position structure |
| CURSOR_SET_TYPE | 0x11 | unsupported | cursor type argument |

## Notes

1. A raw command number is passed with no argument, so only use numbers for commands the driver documents as argument-free; the result reports success or failure only.
2. `BLK_GETGEOME` on a block device is read-only and safe; use it to learn block size before planning writes. `device_read` and `device_write` already call it internally.
3. Class-specific commands not listed here belong to vendor drivers; their argument layouts are unknown to this tool, so they cannot be called safely. Report that limitation instead of guessing.
4. Command success reports the driver return code only. Verify effects with a read or telemetry before claiming the peripheral changed state.
5. When a peripheral needs a class-specific call (PWM period, ADC channel, I2C address, SPI transfer, pin level, sensor rate, LED state), prefer the class tools listed in the `device` skill over `device_control`. They build the driver arguments for you; only the classes compiled into this firmware are in your tool list.
